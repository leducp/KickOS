// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The libc reentrant-state seam. The kernel never names struct _reent and includes no
// newlib header: the user side STATES where the state lives, in a descriptor of plain
// pointers and widths, and the kernel does the acquiring, the priming and the seating
// itself.
//
// WHY THE KERNEL DOES THE WORK RATHER THAN CALLING FOR IT. Where a translating backend
// splits the image, every EL0-reachable leaf carries privileged-execute-never, so the
// kernel may not call app text at all (docs/design-m6-mmu.md, T5b). A seam made of calls
// is therefore not expressible there; a seam made of data is.
//
// WHY EVERY WRITE IS A kmemcpy AND NEVER A TYPED STORE. Two facts, both still true, and
// both were the reason the seam USED to be a call:
//   * struct _reent is the real type of the seated object and of every array element, and
//     it is in scope only on the user side. A kernel store through void** asserts an
//     effective type the object does not have.
//   * on the Xtensa backend the word libc resolves from is a file-scoped static, so LTO
//     may fold its reader against a definition an aliased store never touched.
// kmemcpy answers both. Its bytes carry no effective type, so no alias set is asserted,
// and the address reaching it out of an exported descriptor is an escape the folding has
// to respect. Turning one of these into an assignment reintroduces both. Still true where the
// write goes through the kaccess seam: the innermost copy there is the same kmemcpy.
//
// HOW THE KERNEL LEARNS THE DESCRIPTOR IS THE PART THE LINKER SPLIT REPLACES. Today app
// data sits in the kernel's half, so the kernel names kickos_reent_seam directly and the
// link resolves it out of libkickos_user.a (the RESCAN group in the root CMakeLists). The
// kernel reads it ONCE at boot into storage of its own and never reads the app object
// again, so the split changes how the descriptor is populated and nothing below it.

#ifndef KICKOS_REENT_H
#define KICKOS_REENT_H

#include <stddef.h>

// Global scope deliberately, matching kickos/arch/arch.h: an elaborated `struct arch_aspace*`
// first seen inside namespace kickos would declare a second, unrelated type.
struct arch_aspace;

#if !KICKOS_ARCH_SIM

// The array behind the seam is ONE array indexed by thread slot, so two kernel instances
// would hand the same slot number the same struct _reent. KICKOS_MULTI_INSTANCE depends on
// ARCH_SIM and this block is !KICKOS_ARCH_SIM, so the pair is already exclusive; this
// refuses a hand-built compile whose two macros disagree.
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
#error "KICKOS_MULTI_INSTANCE is set on a build that is not the sim"
#endif

extern "C"
{
    // EVERY MEMBER IS void*/size_t/int SO THE APP CAN INITIALISE IT STATICALLY. Each one
    // is a plain implicit conversion from the object's own type; a cast here would make
    // the object dynamically initialised, and a user-side ctor runs from root_entry, which
    // is after the kernel has already read this.
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

    // Bring a slot to its post-boot contents. CALLED FROM THE SWITCH-IN AND NOWHERE ELSE:
    // it writes hundreds of bytes (240 to 568 across the pinned toolchains) into memory the
    // INCOMING thread owns, so a spawn doing it would pay for that inside the spawn's
    // IrqLock, and once processes have spaces of their own it would land in the spawner's
    // frame at an identical virtual address. Both are answered by doing it after the
    // target's memory view is installed.
    //
    // `space` IS THE INCOMING THREAD'S OWN, and both of these reach it through the kaccess
    // seam rather than through the running translation, so the frame written is that space's
    // whether or not it is the one installed. Null on a board with no translating backend,
    // where both write directly.
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
