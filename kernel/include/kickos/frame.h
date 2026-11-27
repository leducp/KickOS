// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The physical frame allocator, a binary buddy system. Its record lives in the last frames of
// its range, and each free list's links in the first two words of its free blocks.

#ifndef KICKOS_FRAME_H
#define KICKOS_FRAME_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    class FrameAllocator
    {
    public:
        // `granule` must be a power of two of at least two words, with `base` aligned to it. A
        // range too small for its record plus one usable frame is refused. Idempotent.
        bool init(uintptr_t base, size_t size, size_t granule);

        // One frame, the first of the smallest free block, or 0 when none is free.
        uintptr_t alloc();

        // `pages` consecutive frames: the start of the smallest free block holding that many,
        // or, when no free block is that large, the range's first frame if the run is free
        // there. Released with release_run, or frame by frame.
        uintptr_t alloc_run(size_t pages);

        // `pages`, or 0 and nothing changed unless every frame of [addr, addr + pages
        // granules) is allocated and not the record's.
        size_t release_run(uintptr_t addr, size_t pages);

        // Refuses, and changes nothing, unless `addr` is an allocated frame of this range:
        // unaligned, out of range, the record's own, or already free.
        bool release(uintptr_t addr);

        bool is_allocated(uintptr_t addr) const;

        size_t frames_total() const { return usable_; }
        size_t frames_free() const { return free_; }

    private:
        // One byte per frame: 0 inside a block, else the order of the block it heads plus one,
        // with TAG_FREE while free. No two buddies are in the same state, so a wholly
        // allocated span is one block and a release checks it in O(log) steps.
        uint8_t* tags_ = nullptr;
        // Per order, the frame heading the first free block, or NIL.
        size_t* heads_ = nullptr;
        uintptr_t base_ = 0;
        size_t granule_ = 0;
        size_t frames_ = 0;   // frames in the whole range, the record's included
        size_t reserved_ = 0; // the trailing frames the record occupies
        size_t usable_ = 0;   // frames_ - reserved_
        size_t free_ = 0;
        size_t top_ = 0;    // the highest order a block of this range can have
        size_t orders_ = 0; // bit o set while order o has a free block

        bool index_of(uintptr_t addr, size_t* out) const;

        // The block holding frame `i`.
        void find(size_t i, size_t* head, size_t* order) const;

        // The order of the largest block that starts at `first` and ends at or before `end`.
        size_t block_at(size_t first, size_t end) const;

        // Whether each block of [first, first + n) lies in a block in `state`.
        bool spans(size_t first, size_t n, uint8_t state) const;

        // Puts block (i, order) in `state`: splits the block holding it, then merges it with
        // each buddy already in `state`. False, changing nothing, unless the block holding it
        // is in the other state.
        bool flip(size_t i, size_t order, uint8_t state);

        // Puts each block of [first, first + n) in `state`, each in the other state.
        void flip_span(size_t first, size_t n, uint8_t state);

        void push(size_t head, size_t order);
        void unlink(size_t head, size_t order);
    };
}

#endif
