// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS fleet-wide syscall error taxonomy. A syscall that can fail returns its
// error as the NEGATED code (-KOS_Exxx); a handle/count/byte-count success is
// non-negative, so the two never collide. Pointer-returning (ram_alloc) and Hz-returning
// (cpu_clock_hz/set) syscalls stay OUT of this scheme; see their own contracts.
//
// Lives in the kickos_system library alongside cap_index.h; keep it dependency-free.
// Shared verbatim by the kernel dispatch and the userspace wrappers.
//
// Values mirror the common POSIX errno numbers, but only the MAGNITUDE is contract (the
// sign is applied at the return site). Every code is returned NEGATIVE; there is no
// positive success-variant. -KOS_EOWNERDEAD is the one a caller must special-case as
// HELD: the mutex was ACQUIRED (see mutex_lock).

#ifndef KICKOS_SYS_ERRNO_H
#define KICKOS_SYS_ERRNO_H

enum kos_errno
{
    KOS_EPERM = 1,       // privilege denied / missing cap right / not the owner
    KOS_ESRCH = 3,       // reply target gone: a one-shot reply cap's caller is stale (aborted/reused)
    KOS_EIO = 5,         // the DEVICE on the far side of a bus refused or failed the transfer:
                         //   an I2C NACK, a peripheral reporting a wire-level error. Nothing
                         //   about the request is malformed and no deadline passed; the peer
                         //   did not play its part.
    KOS_EBADF = 9,       // handle names nothing valid: bad index, empty, stale gen, wrong type
    KOS_ENOMEM = 12,     // something had to be ALLOCATED and could not be: an object pool, the
                         //   thread pool, the RAM arena, the capability-chunk slab, the caller's
                         //   MPU descriptor budget. A full capability table is KOS_EMFILE, which
                         //   allocates nothing.
    KOS_EFAULT = 14,     // user buffer/pointer not owned by the caller (isolation reject)
    KOS_EBUSY = 16,      // resource held/in-use: close a mutex you own; claim an owned irq line
    KOS_EINVAL = 22,     // malformed argument: bad prio/stack/mask/count/irq line/alignment/size
    KOS_EMFILE = 24,     // ONE task's capability table has no free slot; nothing was allocated.
                         //   Table width is declared demand summed at configure
                         //   (cmake/cap_table.cmake). From kos_call this names the SERVER's
                         //   table, not the caller's: the reply cap is minted into the receiver.
    KOS_EPIPE = 32,      // endpoint has no receiver (dead), or the last one left while parked
    KOS_EDEADLK = 35,    // self/recursive lock, or a lock that would close a wait cycle
    KOS_ENOSYS = 38,     // syscall/arch backend not implemented on this chip (the declining fallback)
    KOS_EOVERFLOW = 75,  // a bounded counter is at its ceiling; the op is refused, not wrapped
    KOS_ENOTSUP = 95,    // the request is well-formed and the backend simply cannot express it:
                         //   a frame format this controller has no encoding for, a rate outside
                         //   its divider's reach. Distinct from KOS_EINVAL, which blames the
                         //   caller, and from KOS_ENOSYS, which says the arch arm is absent.
    KOS_ETIMEDOUT = 110, // a caller-supplied deadline passed before the operation could
                         //   complete, and NOTHING happened: a timed send that expires
                         //   delivered no bytes and left no state behind
    KOS_ECANCELED = 125, // this thread was cancelled: the wait it was in (or was about to
                         //   enter) is abandoned, and the thread is expected to exit
    KOS_EOWNERDEAD = 130 // mutex ACQUIRED but the prior owner died holding it (state may be torn)
};

#endif
