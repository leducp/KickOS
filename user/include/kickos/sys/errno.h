// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS fleet-wide syscall error taxonomy. A syscall that can fail returns
// its error as the NEGATED code (-KOS_Exxx); a handle/count/byte-count success is
// non-negative, so the two never collide (handles are bounded well under INT_MAX
// and counts stay small). Pointer-returning (ram_alloc) and Hz-returning
// (cpu_clock_hz/set) syscalls stay OUT of this scheme -- see their own contracts.
//
// Standalone for now; its final home MAY move to a future kickos_system library
// alongside cap_index.h; keep it dependency-free so that move is a pure
// relocation. Shared verbatim by the kernel dispatch and the userspace wrappers.
//
// Values mirror the common POSIX errno numbers so they read at a glance; only the
// MAGNITUDE is contract (the sign is applied at the return site). Every code is
// returned NEGATIVE; there is no positive success-variant. EOWNERDEAD is the
// robust-mutex case: it is ACQUIRED-with-a-warning, still returned as a negative
// (-KOS_EOWNERDEAD) that a caller must special-case as HELD (see mutex_lock).

#ifndef KICKOS_SYS_ERRNO_H
#define KICKOS_SYS_ERRNO_H

enum kos_errno
{
    KOS_EPERM = 1,       // privilege denied / missing cap right / not the owner
    KOS_EBADF = 9,       // handle names nothing valid: bad index, empty, stale gen, wrong type
    KOS_ENOMEM = 12,     // exhaustion: an object pool / cap table / RAM arena is full
    KOS_EFAULT = 14,     // user buffer/pointer not owned by the caller (isolation reject)
    KOS_EBUSY = 16,      // resource held/in-use: close a mutex you own; claim an owned irq line
    KOS_EINVAL = 22,     // malformed argument: bad prio/stack/mask/count/irq line/alignment/size
    KOS_EPIPE = 32,      // endpoint has no receiver (dead), or the last one left while parked
    KOS_EDEADLK = 35,    // self/recursive lock, or a lock that would close a wait cycle
    KOS_EOWNERDEAD = 130 // mutex ACQUIRED but the prior owner died holding it (state may be torn)
};

#endif
