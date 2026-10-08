// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An unprivileged thread's stack, once it stops being an arena block and becomes frames
// mapped into its task's address space. Three properties the arena could not give it: no
// power-of-two size, no natural alignment, and an UNMAPPED page below it, so an overrun
// faults instead of reaching a neighbour.
//
// THE MAPPING IS TASK-WIDE, the portable floor: a sibling in the same task reaches this
// stack. A thread-scoped grant guarantees access to its holder and nothing about denial to a
// peer.
//
// THE OUTPUT ADDRESS IS THE VIRTUAL ADDRESS, as for the image. The kernel can then write a
// stack it does not have activated (a spawn seating the TLS block before the child's space
// runs): the frame pool answers for the same bytes through the physical map, with no second
// address in the TCB. It also makes the range globally unique, so the guard page below it
// cannot be some other space's mapping.
//
// Compiled to nothing without a translating backend, where a stack stays an arena block.

#ifndef KICKOS_USTACK_H
#define KICKOS_USTACK_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    struct Domain;

#if KICKOS_HAVE_ASPACE

    struct UserStack
    {
        uintptr_t base = 0; // 0 means the allocation failed
        size_t bytes = 0;
    };

    // Stack frames mapped read-write into `d`'s space, plus one frame below them held
    // allocated and left UNMAPPED. Total-or-fail.
    //
    // The whole run is recorded as one VR_USTACK range BEFORE the map, so a reservation
    // landing on the stack or its guard is refused.
    UserStack ustack_alloc(Domain* d, size_t want);

    // `d` must still hold the live space the stack was mapped into: a thread's stack is
    // released before its task's reference is dropped, not at slot reclaim.
    void ustack_free(Domain* d, uintptr_t base, size_t bytes);

    // The pointer the KERNEL reaches a frame-pool run through, or null when `base` names no
    // frame this pool handed out (an arena block, directly dereferenceable).
    //
    // IT DOES NOT SAY WHO OWNS THE RUN: a caller-supplied stack answers as a kernel-allocated
    // one does. Thread::kstack_owned says which.
    void* ustack_kptr(uintptr_t base);

#else

    inline void* ustack_kptr(uintptr_t) { return nullptr; }

#endif
}

#endif
