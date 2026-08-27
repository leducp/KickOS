// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The physical frame allocator. kickos/frame.h carries the contract.

#include <kickos/frame.h>

namespace kickos
{
    namespace
    {
        bool is_pow2(size_t v)
        {
            if (v == 0)
            {
                return false;
            }
            return (v & (v - 1u)) == 0;
        }
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
        // dependent. Rounding up once over-describes the frames it sits on, which is free.
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
        // Marked allocated, so no path has to treat them as special.
        for (size_t i = 0; i < reserved; i++)
        {
            bits_[i / 8u] = static_cast<uint8_t>(bits_[i / 8u] | (1u << (i % 8u)));
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

    uintptr_t FrameAllocator::alloc()
    {
        if (free_ == 0)
        {
            return 0;
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
            for (size_t i = start; i < end; i++)
            {
                uint8_t const mask = static_cast<uint8_t>(1u << (i % 8u));
                if ((bits_[i / 8u] & mask) == 0)
                {
                    bits_[i / 8u] = static_cast<uint8_t>(bits_[i / 8u] | mask);
                    free_--;
                    hint_ = i + 1;
                    return base_ + i * granule_;
                }
            }
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
