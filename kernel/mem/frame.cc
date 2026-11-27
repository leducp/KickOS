// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The physical frame allocator. kickos/frame.h carries the contract.
//
// Every operation is bounded by the range's order count L = log2(frames), whatever the run:
// find, a split and a merge are O(L) each, a span is at most 2L blocks, so alloc() is O(L) and
// alloc_run, release_run and release are O(L^2).

#include <kickos/frame.h>

#include <kickos/extent.h> // is_pow2

namespace kickos
{
    namespace
    {
        constexpr size_t WORD_BYTES = sizeof(size_t);
        constexpr size_t NIL = ~static_cast<size_t>(0);
        constexpr uint8_t TAG_FREE = 0x80u;
        constexpr uint8_t TAG_ORDER = 0x7Fu;

        static_assert(sizeof(size_t) == sizeof(unsigned long), "ctz builtin narrower than a word");

        // Undefined for 0: the word must be tested first.
        size_t ctz_word(size_t v)
        {
            return static_cast<size_t>(__builtin_ctzl(static_cast<unsigned long>(v)));
        }

        // The pool is raw bytes, never a size_t object.
        typedef size_t __attribute__((__may_alias__)) link_word;

        size_t bit(size_t order)
        {
            return static_cast<size_t>(1) << order;
        }
    }

    bool FrameAllocator::init(uintptr_t base, size_t size, size_t granule)
    {
        tags_ = nullptr;
        heads_ = nullptr;
        base_ = 0;
        granule_ = 0;
        frames_ = 0;
        reserved_ = 0;
        usable_ = 0;
        free_ = 0;
        top_ = 0;
        orders_ = 0;

        if (base == 0 or not is_pow2(granule) or granule < 2u * WORD_BYTES)
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
        size_t top = 0;
        while (top + 1u < 8u * WORD_BYTES and bit(top + 1u) <= frames)
        {
            top++;
        }
        size_t const tag_bytes = ((frames + WORD_BYTES - 1u) / WORD_BYTES) * WORD_BYTES;
        size_t const record = tag_bytes + (top + 1u) * WORD_BYTES;
        size_t const reserved = (record + granule - 1u) / granule;
        if (reserved >= frames)
        {
            return false; // capacity 0 would read to a caller as ordinary exhaustion
        }

        base_ = base;
        granule_ = granule;
        frames_ = frames;
        reserved_ = reserved;
        usable_ = frames - reserved;
        free_ = usable_;
        top_ = top;
        tags_ = reinterpret_cast<uint8_t*>(base + usable_ * granule);
        heads_ = reinterpret_cast<size_t*>(tags_ + tag_bytes);
        for (size_t i = 0; i < tag_bytes; i++)
        {
            tags_[i] = 0;
        }
        for (size_t o = 0; o <= top; o++)
        {
            heads_[o] = NIL;
        }
        // Greedy blocks are maximal, so neither span holds two buddies.
        size_t o = 0;
        for (size_t i = 0; i < usable_; i = i + bit(o))
        {
            o = block_at(i, usable_);
            tags_[i] = static_cast<uint8_t>(TAG_FREE | (o + 1u));
            push(i, o);
        }
        for (size_t i = usable_; i < frames; i = i + bit(o))
        {
            o = block_at(i, frames);
            tags_[i] = static_cast<uint8_t>(o + 1u);
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

    // Inlined, with push, unlink and flip, so that alloc() and release() call nothing: they end
    // the deepest x86_64 kernel stack chains (SYSK, EXITK).
    inline __attribute__((always_inline))
    void FrameAllocator::find(size_t i, size_t* head, size_t* order) const
    {
        // The first head met going up is the block's: a smaller order's candidate is either
        // inside the block (tag 0) or the block's own head.
        for (size_t o = 0; o <= top_; o++)
        {
            size_t const h = i & ~(bit(o) - 1u);
            size_t const t = tags_[h] & TAG_ORDER;
            if (t > o)
            {
                *head = h;
                *order = t - 1u;
                return;
            }
        }
        *head = i;
        *order = 0;
    }

    inline __attribute__((always_inline))
    void FrameAllocator::push(size_t head, size_t order)
    {
        link_word* const node = reinterpret_cast<link_word*>(base_ + head * granule_);
        size_t const next = heads_[order];
        node[0] = next;
        node[1] = NIL;
        if (next != NIL)
        {
            reinterpret_cast<link_word*>(base_ + next * granule_)[1] = head;
        }
        heads_[order] = head;
        orders_ = orders_ | bit(order);
    }

    inline __attribute__((always_inline))
    void FrameAllocator::unlink(size_t head, size_t order)
    {
        link_word const* const node = reinterpret_cast<link_word const*>(base_ + head * granule_);
        size_t const next = node[0];
        size_t const prev = node[1];
        if (prev == NIL)
        {
            heads_[order] = next;
        }
        else
        {
            reinterpret_cast<link_word*>(base_ + prev * granule_)[0] = next;
        }
        if (next != NIL)
        {
            reinterpret_cast<link_word*>(base_ + next * granule_)[1] = prev;
        }
        if (heads_[order] == NIL)
        {
            orders_ = orders_ & ~bit(order);
        }
    }

    inline __attribute__((always_inline))
    bool FrameAllocator::flip(size_t i, size_t order, uint8_t state)
    {
        uint8_t const was = static_cast<uint8_t>(state ^ TAG_FREE);
        size_t h = 0;
        size_t o = 0;
        find(i, &h, &o);
        if ((tags_[h] & TAG_FREE) != was or o < order)
        {
            return false;
        }
        if (was == TAG_FREE)
        {
            unlink(h, o);
        }
        // Each half left behind keeps the old state; its buddy holds `i`, so it cannot merge.
        while (o > order)
        {
            o--;
            size_t rest = h + bit(o);
            if (i >= rest)
            {
                rest = h;
                h = h + bit(o);
            }
            tags_[rest] = static_cast<uint8_t>(was | (o + 1u));
            if (was == TAG_FREE)
            {
                push(rest, o);
            }
        }
        while (o < top_)
        {
            // tags_[frames_] is padding or the low byte of heads_[0]. A buddy cut short by the end
            // heads a smaller block, so it never matches.
            size_t const buddy = h ^ bit(o);
            if (buddy >= frames_ or tags_[buddy] != (state | (o + 1u)))
            {
                break;
            }
            if (state == TAG_FREE)
            {
                unlink(buddy, o);
            }
            h = h & ~bit(o);
            tags_[h + bit(o)] = 0;
            o++;
        }
        tags_[h] = static_cast<uint8_t>(state | (o + 1u));
        if (state == TAG_FREE)
        {
            push(h, o);
        }
        return true;
    }

    size_t FrameAllocator::block_at(size_t first, size_t end) const
    {
        size_t o = 0;
        while (o < top_ and (first & bit(o)) == 0 and bit(o + 1u) <= end - first)
        {
            o++;
        }
        return o;
    }

    bool FrameAllocator::spans(size_t first, size_t n, uint8_t state) const
    {
        size_t const end = first + n;
        size_t o = 0;
        for (size_t i = first; i < end; i = i + bit(o))
        {
            o = block_at(i, end);
            size_t h = 0;
            size_t q = 0;
            find(i, &h, &q);
            if ((tags_[h] & TAG_FREE) != state or q < o)
            {
                return false;
            }
        }
        return true;
    }

    void FrameAllocator::flip_span(size_t first, size_t n, uint8_t state)
    {
        size_t const end = first + n;
        size_t o = 0;
        for (size_t i = first; i < end; i = i + bit(o))
        {
            o = block_at(i, end);
            (void)flip(i, o, state);
        }
    }

    uintptr_t FrameAllocator::alloc()
    {
        if (orders_ == 0)
        {
            return 0;
        }
        size_t const h = heads_[ctz_word(orders_)];
        (void)flip(h, 0, 0);
        free_--;
        return base_ + h * granule_;
    }

    uintptr_t FrameAllocator::alloc_run(size_t pages)
    {
        if (pages == 0 or free_ < pages)
        {
            return 0;
        }
        size_t k = 0;
        while (bit(k) < pages)
        {
            k++;
        }
        size_t first = NIL;
        if (k <= top_ and (orders_ >> k) != 0)
        {
            first = heads_[k + ctz_word(orders_ >> k)];
        }
        else if (spans(0, pages, TAG_FREE))
        {
            first = 0;
        }
        if (first == NIL)
        {
            return 0;
        }
        flip_span(first, pages, 0);
        free_ -= pages;
        return base_ + first * granule_;
    }

    size_t FrameAllocator::release_run(uintptr_t addr, size_t pages)
    {
        size_t first = 0;
        if (pages == 0 or not index_of(addr, &first) or first >= usable_
            or pages > usable_ - first)
        {
            return 0;
        }
        if (not spans(first, pages, 0))
        {
            return 0;
        }
        flip_span(first, pages, TAG_FREE);
        free_ += pages;
        return pages;
    }

    bool FrameAllocator::release(uintptr_t addr)
    {
        size_t i = 0;
        if (not index_of(addr, &i) or i >= usable_ or not flip(i, 0, TAG_FREE))
        {
            return false;
        }
        free_++;
        return true;
    }

    bool FrameAllocator::is_allocated(uintptr_t addr) const
    {
        size_t i = 0;
        if (not index_of(addr, &i))
        {
            return false;
        }
        size_t h = 0;
        size_t o = 0;
        find(i, &h, &o);
        return (tags_[h] & TAG_FREE) == 0;
    }
}
