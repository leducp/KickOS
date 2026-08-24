// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The libc reentrant-state seam. The kernel never names struct _reent and includes no
// newlib header: it moves an opaque pointer through three calls the USER side defines,
// resolved at link out of libkickos_user.a (the RESCAN link group in the root
// CMakeLists), so there is no syscall and no registration call.

#ifndef KICKOS_REENT_H
#define KICKOS_REENT_H

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
    // The struct _reent a thread-pool slot owns, UNINITIALISED. A TCB the pool does not
    // own (idle) passes a negative index and gets libc's process-wide state, which is what
    // the seam held before the first switch.
    void* kickos_reent_acquire(int slot);

    // Bring a slot's state to its post-boot contents. Hundreds of bytes (284 to 512 across
    // the pinned toolchains), so it must NOT run inside a spawn's IrqLock; the caller runs
    // it after the lock drops and before the thread is made READY. A no-op on the
    // process-wide state, which libc initialised itself.
    void kickos_reent_init(void* reent);

    // Make `reent` the state libc resolves from, on every switch. TYPED ON THE USER SIDE
    // on purpose: the object is a struct _reent* and the kernel cannot name that type, so
    // a kernel store through a void** would be a strict-aliasing violation, and on the
    // Xtensa backend, where the word is a file-scoped static, LTO may fold the reader
    // against a definition the aliased store never touched.
    void kickos_reent_seat(void* reent);
}

#endif

#endif
