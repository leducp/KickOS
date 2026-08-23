// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The libc reentrant-state seam. The kernel never names struct _reent and includes no
// newlib header: it moves an opaque pointer into a word the USER side names, resolved at
// link out of libkickos_user.a (the RESCAN link group in the root CMakeLists), so there is
// no syscall and no registration call.

#ifndef KICKOS_REENT_H
#define KICKOS_REENT_H

#if defined(KICKOS_LIBC_REENT) && KICKOS_LIBC_REENT

// The array behind the seam is ONE array indexed by thread slot, so two kernel instances
// would hand the same slot number the same struct _reent. Kconfig already keeps the two
// apart (one depends on ARCH_SIM, the other on !ARCH_SIM); this refuses a build that
// reached the compiler without going through it.
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
#error "KICKOS_LIBC_REENT and KICKOS_MULTI_INSTANCE cannot both be on"
#endif

extern "C"
{
    // The word libc resolves its state through: &_impure_ptr everywhere but Xtensa, whose
    // newlib goes through __getreent() instead. Strong on both sides on purpose. A weak
    // default here would be an archive member the linker need never extract, and the
    // feature would then be a silent no-op instead of a link error; it would also fail
    // check_seam_defaults.sh leg 4.
    extern void* const kickos_reent_slot;

    // The initialised struct _reent for a thread-pool slot. A TCB the pool does not own
    // (idle) passes a negative index and gets libc's process-wide state, which is what the
    // slot held before the first switch.
    void* kickos_reent_acquire(int slot);
}

#endif

#endif
