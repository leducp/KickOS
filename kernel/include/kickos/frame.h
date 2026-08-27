// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The physical frame allocator: a range handed out one granule at a time.
//
// The granule is a parameter and no figure appears here (F7).
//
// The bitmap lives in the first frames of the range it describes.

#ifndef KICKOS_FRAME_H
#define KICKOS_FRAME_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    class FrameAllocator
    {
    public:
        // Describes [base, base + size) in units of `granule`, which must be a power of two
        // with `base` aligned to it. A range too small for its own bitmap plus one usable
        // frame is refused. Idempotent.
        bool init(uintptr_t base, size_t size, size_t granule);

        // A frame's base address, or 0 when exhausted.
        uintptr_t alloc();

        // `pages` consecutive frames, or 0 when no run that long is free. The run's base
        // address is returned; every frame in it must be released individually.
        uintptr_t alloc_run(size_t pages);

        // Refuses, and changes nothing, unless `addr` is an allocated frame of this range:
        // unaligned, out of range, the bitmap's own, or already free.
        bool release(uintptr_t addr);

        // The bitmap's own frames read as allocated.
        bool is_allocated(uintptr_t addr) const;

        size_t frames_total() const { return usable_; }
        size_t frames_free() const { return free_; }

    private:
        // Bit set == allocated, indexed by frame number from `base_` with the bitmap's own
        // frames included.
        uint8_t* bits_ = nullptr;
        uintptr_t base_ = 0;
        size_t granule_ = 0;
        size_t frames_ = 0;   // frames in the whole range, bitmap included
        size_t reserved_ = 0; // the leading frames the bitmap occupies
        size_t usable_ = 0;   // frames_ - reserved_
        size_t free_ = 0;
        // A hint and never an answer: alloc() wraps past it and release() moves it back, so a
        // stale value costs a scan, never a frame.
        size_t hint_ = 0;

        bool index_of(uintptr_t addr, size_t* out) const;

        // A size_t-wide window on the bitmap, frame `widx * 8 * sizeof(size_t)` at its lowest
        // bit whatever the host's byte order. Bits at and past `frames_` read as allocated.
        // Reads no byte past the bitmap's own length. `widx` must name a word holding a live
        // frame.
        size_t scan_word(size_t widx) const;

        // Marks frame `j` allocated and returns its address; the hint follows it.
        uintptr_t take_one(size_t j);

        uintptr_t take_run(size_t first, size_t pages);
    };
}

#endif
