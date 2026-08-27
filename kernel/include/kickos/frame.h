// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The physical frame allocator: a range handed out one granule at a time.
//
// The granule is a PARAMETER and no figure appears here, per F7: it is the arch's answer,
// and a kernel that believed 4 KiB would make SPARC v9's 8 KiB page a kernel change.
//
// The bitmap lives in the first frames of the range it describes, so the type costs no .bss
// on the boards that compile it without ever constructing one.
//
// A bitmap rather than a free list threaded through the free frames: a list cannot answer
// "is this frame already free" without walking itself, and a double release is refused as a
// correctness property.

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
        // frame is refused: zero capacity would read to a caller as ordinary exhaustion.
        // Idempotent.
        bool init(uintptr_t base, size_t size, size_t granule);

        // A frame's base address, or 0 when exhausted.
        uintptr_t alloc();

        // `pages` CONSECUTIVE frames, or 0 when no run that long is free. The run's base
        // address is returned; every frame in it must be released individually, this keeping
        // release() the only accounting there is.
        //
        // A run and not a page walk, because the caller that needs one needs the whole run
        // to have ONE kernel address: a thread's stack is written by the kernel through the
        // physical map before its own space is ever the running one, and a per-page walk
        // there would put a translation in front of a plain memcpy.
        uintptr_t alloc_run(size_t pages);

        // Refuses, and changes nothing, unless `addr` is an allocated frame of this range:
        // unaligned, out of range, the bitmap's own, or ALREADY FREE.
        bool release(uintptr_t addr);

        // The bitmap's own frames read as allocated.
        bool is_allocated(uintptr_t addr) const;

        size_t frames_total() const { return usable_; }
        size_t frames_free() const { return free_; }

    private:
        // Bit set == allocated, indexed by frame number from `base_` with the bitmap's own
        // frames included, so an index is never translated between two spaces.
        uint8_t* bits_ = nullptr;
        uintptr_t base_ = 0;
        size_t granule_ = 0;
        size_t frames_ = 0;   // frames in the whole range, bitmap included
        size_t reserved_ = 0; // the leading frames the bitmap occupies
        size_t usable_ = 0;   // frames_ - reserved_
        size_t free_ = 0;
        // A hint and never an answer: alloc() wraps past it and release() moves it back, so a
        // stale value costs a scan rather than a frame.
        size_t hint_ = 0;

        bool index_of(uintptr_t addr, size_t* out) const;
    };
}

#endif
