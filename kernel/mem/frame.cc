// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The physical frame allocator. kickos/frame.h carries the contract.

#include <kickos/frame.h>

#include <kickos/extent.h> // is_pow2

namespace kickos
{
    namespace
    {
        constexpr size_t WORD_BYTES = sizeof(size_t);
        constexpr size_t WORD_BITS = WORD_BYTES * 8u;
        constexpr size_t ALL_SET = ~static_cast<size_t>(0);

        // A size_t wider than __builtin_ctzl's unsigned long would truncate the scanned word
        // and report a bit that is not the lowest one set.
        static_assert(sizeof(size_t) == sizeof(unsigned long), "ctz builtin narrower than a word");

        // Undefined for 0; every call site tests the word first.
        size_t ctz_word(size_t v)
        {
            return static_cast<size_t>(__builtin_ctzl(static_cast<unsigned long>(v)));
        }

        // Bit i of the bitmap is bits_[i / 8] & (1 << (i % 8)), so frame order equals ascending
        // bit order in a loaded word only where byte 0 lands in the word's low bits: the
        // little-endian load. The big-endian arm writes the byte weights out instead.
        // may_alias because the bitmap is addressed as bytes everywhere else, and aligned(1)
        // because init() accepts a granule of 1, which leaves `bits_` byte-aligned.
#if defined(__BYTE_ORDER__) and defined(__ORDER_LITTLE_ENDIAN__) \
    and __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        typedef size_t __attribute__((__may_alias__, __aligned__(1))) word_load;

        size_t word_at(uint8_t const* p)
        {
            return *reinterpret_cast<word_load const*>(p);
        }
#elif defined(__BYTE_ORDER__) and defined(__ORDER_BIG_ENDIAN__) \
    and __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        size_t word_at(uint8_t const* p)
        {
            size_t w = 0;
            for (size_t k = 0; k < WORD_BYTES; k++)
            {
                w |= static_cast<size_t>(p[k]) << (8u * k);
            }
            return w;
        }
#else
#error "kernel/mem/frame.cc: the bitmap's word scan needs a known byte order"
#endif
    }

    bool FrameAllocator::init(uintptr_t base, size_t size, size_t granule)
    {
        bits_ = nullptr;
        base_ = 0;
        granule_ = 0;
        frames_ = 0;
        reserved_ = 0;
        usable_ = 0;
        free_ = 0;
        hint_ = 0;

        if (base == 0 or not is_pow2(granule))
        {
            return false;
        }
        if ((base & (granule - 1u)) != 0)
        {
            return false;
        }
        size_t const frames = size / granule;
        if (frames == 0)
        {
            return false;
        }
        // The bitmap describes itself, so its size and the space it occupies are mutually
        // dependent.
        size_t const bitmap_bytes = (frames + 7u) / 8u;
        size_t const reserved = (bitmap_bytes + granule - 1u) / granule;
        if (reserved >= frames)
        {
            return false; // capacity 0 would read to a caller as ordinary exhaustion
        }

        bits_ = reinterpret_cast<uint8_t*>(base);
        base_ = base;
        granule_ = granule;
        frames_ = frames;
        reserved_ = reserved;
        usable_ = frames - reserved;
        free_ = usable_;
        hint_ = reserved;

        for (size_t i = 0; i < bitmap_bytes; i++)
        {
            bits_[i] = 0;
        }
        // The bitmap's own frames are marked allocated, so no path has to treat them as
        // special. `reserved < frames` keeps the partial byte inside the bitmap.
        size_t const whole = reserved / 8u;
        for (size_t i = 0; i < whole; i++)
        {
            bits_[i] = 0xFFu;
        }
        size_t const rest = reserved % 8u;
        if (rest != 0)
        {
            bits_[whole] = static_cast<uint8_t>((1u << rest) - 1u);
        }
        return true;
    }

    bool FrameAllocator::index_of(uintptr_t addr, size_t* out) const
    {
        if (granule_ == 0 or addr < base_)
        {
            return false;
        }
        uintptr_t const off = addr - base_;
        if ((off & (granule_ - 1u)) != 0)
        {
            return false;
        }
        size_t const i = static_cast<size_t>(off / granule_);
        if (i >= frames_)
        {
            return false;
        }
        *out = i;
        return true;
    }

    size_t FrameAllocator::scan_word(size_t widx) const
    {
        size_t const first = widx * WORD_BITS;
        if (frames_ - first >= WORD_BITS)
        {
            return word_at(bits_ + widx * WORD_BYTES);
        }
        // The bitmap is a whole number of bytes, so bits at and past frames_ exist and read as
        // zero. Forced to 1 here: bits at and past frames_ read as allocated, which keeps the
        // scan inside the range and covers every byte past the bitmap's own length, so the
        // loop stops there rather than reads it.
        size_t const bitmap_bytes = (frames_ + 7u) / 8u;
        size_t w = ALL_SET << (frames_ - first);
        for (size_t k = 0; k < WORD_BYTES; k++)
        {
            size_t const b = widx * WORD_BYTES + k;
            if (b >= bitmap_bytes)
            {
                break;
            }
            w |= static_cast<size_t>(bits_[b]) << (8u * k);
        }
        return w;
    }

    uintptr_t FrameAllocator::take_one(size_t j)
    {
        bits_[j / 8u] = static_cast<uint8_t>(bits_[j / 8u] | (1u << (j % 8u)));
        free_--;
        hint_ = j + 1u;
        return base_ + j * granule_;
    }

    uintptr_t FrameAllocator::take_run(size_t first, size_t pages)
    {
        for (size_t j = first; j < first + pages; j++)
        {
            bits_[j / 8u] = static_cast<uint8_t>(bits_[j / 8u] | (1u << (j % 8u)));
        }
        free_ -= pages;
        if (hint_ < first + pages)
        {
            hint_ = first + pages;
        }
        return base_ + first * granule_;
    }

    uintptr_t FrameAllocator::alloc()
    {
        if (free_ == 0)
        {
            return 0;
        }
        if (hint_ < frames_ and (bits_[hint_ / 8u] & (1u << (hint_ % 8u))) == 0)
        {
            return take_one(hint_);
        }
        // Two passes: hint to end, then reserved to hint. free_ is non-zero, so one finds.
        for (int pass = 0; pass < 2; pass++)
        {
            size_t start = hint_;
            size_t end = frames_;
            if (pass == 1)
            {
                start = reserved_;
                end = hint_;
            }
            size_t i = start;
            while (i < end)
            {
                size_t const first = (i / WORD_BITS) * WORD_BITS;
                // The last word of a pass runs past `end` and needs no upper mask: bits at
                // and past frames_ read as allocated, and in the wrap pass everything from
                // hint_ up is allocated or the first pass would have returned it.
                size_t const free_bits = ~scan_word(i / WORD_BITS) & (ALL_SET << (i - first));
                if (free_bits != 0)
                {
                    return take_one(first + ctz_word(free_bits));
                }
                i = first + WORD_BITS;
            }
        }
        return 0;
    }

    uintptr_t FrameAllocator::alloc_run(size_t pages)
    {
        if (pages == 0 or free_ < pages)
        {
            return 0;
        }
        // One forward sweep from the bitmap's own frames, and not from hint_: hint_ chases
        // single allocations, so starting there would refuse a run that fits below it while
        // the count says one is free.
        //
        // `run` counts the free frames immediately below `p` and is under `pages` at the top
        // of every word, the completing frame being taken the moment the total reaches it.
        size_t run = 0;
        size_t i = reserved_;
        while (i < frames_)
        {
            size_t const first = (i / WORD_BITS) * WORD_BITS;
            size_t const above = ALL_SET << (i - first);
            size_t taken = scan_word(i / WORD_BITS) & above;
            if (taken == above)
            {
                run = 0;
                i = first + WORD_BITS;
                continue;
            }
            size_t p = i;
            while (taken != 0)
            {
                size_t const q = first + ctz_word(taken);
                if (run + (q - p) >= pages)
                {
                    return take_run(p - run, pages);
                }
                run = 0;
                p = q + 1u;
                taken &= taken - 1u;
            }
            if (run + (first + WORD_BITS - p) >= pages)
            {
                return take_run(p - run, pages);
            }
            run += first + WORD_BITS - p;
            i = first + WORD_BITS;
        }
        return 0;
    }

    bool FrameAllocator::release(uintptr_t addr)
    {
        size_t i = 0;
        if (not index_of(addr, &i))
        {
            return false;
        }
        if (i < reserved_)
        {
            return false; // the bitmap's own frames were never handed out
        }
        uint8_t const mask = static_cast<uint8_t>(1u << (i % 8u));
        if ((bits_[i / 8u] & mask) == 0)
        {
            return false; // already free: a double release, and it changes nothing
        }
        bits_[i / 8u] = static_cast<uint8_t>(bits_[i / 8u] & ~mask);
        free_++;
        if (i < hint_)
        {
            hint_ = i;
        }
        return true;
    }

    bool FrameAllocator::is_allocated(uintptr_t addr) const
    {
        size_t i = 0;
        if (not index_of(addr, &i))
        {
            return false;
        }
        return (bits_[i / 8u] & (1u << (i % 8u))) != 0;
    }
}
