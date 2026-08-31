// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The libc reentrant-state seam. The kernel never names struct _reent and includes no newlib
// header: the user side STATES where the state lives, in a descriptor of plain pointers and
// widths, and the kernel does the acquiring, the priming and the seating itself.
//
// KICKOS_LIBC_REENT names three postures. A cross toolchain gives the kernel newlib's per-thread
// state to own. The sim's libc is the HOST's and owns its own. The x86_64 UEFI toolchain links no
// C library, so nothing below is compiled; do NOT answer that posture with a descriptor of zero
// slots, whose seat word is an address the kernel would still write through.
//
// The seam is DATA: where a translating backend splits the image every EL0-reachable leaf carries
// privileged-execute-never, so the kernel may not call app text at all.
//
// EVERY WRITE IS A kmemcpy AND NEVER A TYPED STORE. struct _reent is in scope only on the user
// side, so a kernel store through void** asserts an effective type the object does not have; and
// on Xtensa the word libc resolves from is a file-scoped static, which LTO may fold against a
// definition an aliased store never touched. Turning one of these into an assignment reintroduces
// both.
//
// The descriptor is read ONCE at boot into storage of the kernel's own.

#ifndef KICKOS_REENT_H
#define KICKOS_REENT_H

#include <stddef.h>

// Global scope deliberately, matching kickos/arch/arch.h: an elaborated `struct arch_aspace*`
// first seen inside namespace kickos would declare a second, unrelated type.
struct arch_aspace;

#if KICKOS_LIBC_REENT

// The array behind the seam is ONE array indexed by thread slot, so two kernel instances
// would hand the same slot number the same struct _reent.
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
#error "KICKOS_MULTI_INSTANCE is set on a build that is not the sim"
#endif

extern "C"
{
    // EVERY MEMBER IS void*/size_t/int SO THE APP CAN INITIALISE IT STATICALLY. A cast here
    // would make the object dynamically initialised, and a user-side ctor runs from
    // root_entry, after the kernel has already read this.
    struct KickosReentSeam
    {
        void* slots;   // element 0 of the per-thread-slot array
        void* shared;  // the process-wide state libc built for itself
        void* seat;    // address of the word libc resolves its state from
        size_t stride; // one element's width
        int count;     // elements in `slots`
    };

    extern KickosReentSeam const kickos_reent_seam;
}

namespace kickos
{
    // Boot. Must precede the first thread_create, which acquires out of the descriptor.
    void reent_seam_read(void);

    // The state a thread-pool slot owns, UNPRIMED. A TCB the pool does not own (idle)
    // passes a negative index and gets the process-wide state, which is what the seat held
    // before the first switch.
    void* reent_state_for_slot(int slot);

    // Bring a slot to its post-boot contents. SWITCH-IN AND NOWHERE ELSE: it writes hundreds
    // of bytes into memory the INCOMING thread owns.
    //
    // `space` is the incoming thread's own, and both of these reach it through the kaccess seam
    // rather than through the running translation. Null on a board with no translating backend.
    //
    // A REFUSED WRITE SLAYS THE INCOMING THREAD AND MUST NOT PANIC: both run with
    // kernel().current already assigned to that thread, and switch_book's claim three statements
    // past the call is what redirects the resume. On sched::start's path nothing claims a resume,
    // so a kill taken at the first switch lands at that thread's next syscall entry.
    void reent_prime(struct arch_aspace* space, void* state);

    // Make `state` the one libc resolves from. Runs on EVERY switch.
    void reent_seat(struct arch_aspace* space, void* state);

#if defined(KICKOS_ENABLE_SELFTEST)
    // Times either of the two above wrote the app half for a thread whose memory view was not
    // installed. Must be 0: the switch path is what refuses to call them in that posture, and
    // this counts from the other side of that guard (kickos/aspace.h, aspace_seated_for).
    size_t reent_unseated_writes(void);
#endif
}

#endif

#endif
