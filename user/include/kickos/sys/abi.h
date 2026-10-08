// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS syscall ABI shared by userspace stubs and the kernel dispatch table. Numbers
// are stable contract; argument packing is uintptr_t-wide.
//
// The `op` selectors and result encodings the *_PROBE entries below name are in
// <kickos/sys/abi_probe.h>, which this header does not include.

#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/cap_index.h> // KOS_CAP_NONE, KOS_CAP_AUTHORITY, the well-known indices
#include <kickos/sys/cap_rights.h> // kos_cap_rights, kos_cap_authority
#include <kickos/sys/errno.h> // KOS_E* taxonomy: failures return -KOS_Exxx (see below)
#include <kickos/sys/exit_status.h> // KOS_EXIT_FAULT, KOS_EXIT_CANCELLED (generated)
#include <kickos/sys/static_assert.h> // KOS_STATIC_ASSERT

// Return-encoding contract (see errno.h). A syscall that can fail returns its error as
// -KOS_Exxx (negative); success is a non-negative byte-count / count, so the two are
// collision-free. Exceptions, all outside the scheme: ram_alloc returns a pointer, 0/NULL on
// ANY failure, cpu_clock_hz / cpu_clock_set return a u64 Hz and periph_clock_hz a u32 Hz,
// each with a 0 == cannot/unknown sentinel.

// A capability handle. 16 index bits + 16 generation bits, so a live handle spends the
// WHOLE 32-bit word and may have bit 31 set: `h < 0` is not an error test on a capability,
// and every capability-MINTING call returns a status and delivers the handle through an
// out-parameter.
typedef uint32_t kos_cap_t;

// A thread handle: 16 index bits + 16 generation bits over the THREAD pool, whose slots and
// generations are unrelated to kos_cap_t's. Both are plain 32-bit words, so the compiler
// will NOT catch a cap handle passed where a thread handle belongs; it just resolves against
// the wrong table. A slot aged past 32768 reclaims mints a handle with bit 31 set, so
// `h < 0` is not an error test here either.
typedef uint32_t kos_thread_t;

// "No thread". The thread pool never seats the all-ones index, so no generation can mint
// this word.
#define KOS_THREAD_NONE 0xFFFFFFFFu

// A task handle: 16 generation bits over a BIASED index into the task pool, whose slots are
// unrelated to either pool above. The bias is what makes the all-zero word unmintable.
typedef uint32_t kos_task_t;

// "No task", and the spawn default: the child is a thread of the spawner's task, sharing its
// memory domain. A spawn that brings its own mem_base, or that changes privilege, gets an
// implicit task holding itself instead.
#define KOS_TASK_NONE 0u

// KOS_EXIT_FAULT, from <kickos/sys/exit_status.h>, is the exit code a thread killed by a CPU fault
// reports: its task's status (kos_task_exit_status), and the process status when it was the last
// thread live. A clean kos_exit(139) aliases it. KOS_EXIT_CANCELLED is the code a CANCELLED thread
// reports: the kernel ends it, so it never picks a code of its own. Also the status of a task
// whose members were all cancelled.

enum kos_syscall_nr
{
    KOS_SYS_KCONSOLE_WRITE = 1, // (buf, len)            -> bytes written, WHICH CAN BE SHORT
                                //   where the console took part or a page went away
                                //   mid-stream, or with no input byte completed -KOS_EFAULT
                                //   (bad buffer), -KOS_EBUSY (the caller's stdout takes it
                                //   now), -KOS_ECANCELED (cancelled while waiting) or, for a
                                //   task that set O_NONBLOCK (KOS_SYS_TASK_NONBLOCK),
                                //   -KOS_ETIMEDOUT where it would have to wait. Waits, with no
                                //   bound, for room in the console, for a peer node's claim
                                //   on a shared UART, and while a dead console driver's task
                                //   awaits its reclaim
    KOS_SYS_YIELD = 2,          // ()                    -> 0
    KOS_SYS_SLEEP_NS = 3,       // (ns_lo, ns_hi)        -> 0
    KOS_SYS_SEM_CREATE = 4,     // (initial, kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM sem pool,
                                //   EMFILE caller's cap table, EAGAIN task's sem budget,
                                //   EINVAL/EFAULT)
    KOS_SYS_SEM_WAIT = 5,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EACCES/-KOS_ECANCELED
    KOS_SYS_SEM_POST = 6,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EACCES, or -KOS_EOVERFLOW
                                //   with no waiter and the count at KOS_SEM_COUNT_MAX
    KOS_SYS_HANDLE_CLOSE = 17,  // (cap)   -> 0, -KOS_EBADF (bad cap), -KOS_EBUSY (own a held mutex)
    KOS_SYS_THREAD_CREATE = 7,   // (kos_thread_params*, kos_thread_t* out) -> 0, or -KOS_E*
                                //   (EINVAL/EFAULT/EPERM/EACCES/EBADF/EBUSY/ENOMEM/EAGAIN/
                                //   EOVERFLOW/ENOTSUP). A RAM range it names, data grant,
                                //   stack or memory window, answers ENOTSUP for a memory type
                                //   this chip cannot honour, then EINVAL where no one region
                                //   descriptor names it, then EPERM, then EBUSY. Across
                                //   arguments the first refusal answers, in the order:
                                //   priority and privilege, the stack, the data grant, the
                                //   windows, the authority word, the capabilities
    KOS_SYS_EXIT = 8,           // (code)                -> does not return. Ends the calling
                                //   thread, or the SYSTEM when the caller is root, which
                                //   needs KOS_AUTH_SYSTEM for it and panics without.
    KOS_SYS_IRQ_INJECT = 9,     // (irq)                 -> 0, -KOS_EINVAL, or -KOS_EPERM
                                //   for a line the kernel dispatches itself (self-test only)
    KOS_SYS_GUARD_ADDR = 10,    // ()  -> protected probe addr (self-test only)
    KOS_SYS_NOTIFY_CREATE = 11, // (kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM notification
                                //   pool, EMFILE caller's cap table, EAGAIN task's
                                //   notification budget, EINVAL/EFAULT out-ptr, EPERM)
    KOS_SYS_CLOCK_NOW = 12,     // ()  -> monotonic nanoseconds (u64, in registers; cannot fail)
    KOS_SYS_RAM_ALLOC = 13,     // (size)                -> user-RAM ptr, or 0/NULL on ANY failure
    KOS_SYS_IRQ_CLAIM = 14,     // (line, flags, kos_cap_t* out) -> 0, or -KOS_E*: EPERM (lacks
                                //   KOS_AUTH_IRQ, or the arch dispatches the line to a kernel
                                //   vector of its own and no holder ever frees it), EINVAL
                                //   (line/flags/out-ptr), EFAULT (out-ptr), EBUSY (line owned),
                                //   EAGAIN (line still retiring from its last holder, or the
                                //   task's binding budget), ENOMEM (binding pool), EMFILE (cap
                                //   table)
    KOS_SYS_NOTIFY_WAIT = 15,   // (notify_cap, accept mask, timeout_us, uint32_t* out_bits)
                                //   -> 0 with the consumed bits in *out_bits, or -KOS_E*:
                                //   EBADF, EACCES (cap lacks WAIT), EPERM (the caller is not
                                //   the bound thread), EINVAL (an empty mask, or a bad out-ptr),
                                //   EFAULT (out-ptr), ETIMEDOUT, ECANCELED. KOS_TIMEOUT_NONE
                                //   waits forever
    KOS_SYS_IRQ_ACK = 16,       // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EACCES (cap lacks WAIT)
                                //   / -KOS_EINVAL (the line signals no notification)
    KOS_SYS_IRQ_SPURIOUS = 18,  // ()  -> count of IRQs on unbound lines (self-test only)
    KOS_SYS_DIAG_LED_SET = 19,  // (on)                  -> 0 (kernel diagnostic LED)
    KOS_SYS_DIAG_LED_TOGGLE = 20, // ()                  -> 0 (kernel diagnostic LED)
    KOS_SYS_IRQ_UNMASK = 21,    // (irq)  -> 0, or -KOS_E* (EPERM/EINVAL; self-test only)
    KOS_SYS_CPU_CLOCK_HZ = 22,  // ()  -> running core clock in Hz (u64), 0 if unknown (NO KOS_E*)
    KOS_SYS_MUTEX_CREATE = 23,  // (kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM mutex pool, EMFILE
                                //   caller's cap table, EAGAIN task's mutex budget,
                                //   EINVAL/EFAULT)
    KOS_SYS_MUTEX_LOCK = 24,    // (cap)  -> 0 held; -KOS_EOWNERDEAD held-but-owner-died; -KOS_EBADF
                                //   / -KOS_EDEADLK NOT held (see the wrapper decl for the caveat)
    KOS_SYS_MUTEX_UNLOCK = 25,  // (cap)  -> 0, -KOS_EBADF (bad cap), -KOS_EPERM (caller not owner)
    KOS_SYS_ENDPOINT_CREATE = 26, // (kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM endpoint pool,
                                  //   EMFILE caller's cap table, EAGAIN task's endpoint
                                  //   budget, EINVAL/EFAULT)
    KOS_SYS_SEND = 27,          // (cap, buf, len) -> bytes transferred, or -KOS_E*: EINVAL (len
                                //   above KOS_EP_MSG_MAX, rejected and never clamped), EFAULT
                                //   (bad buffer), EBADF/EACCES (bad cap / no SIGNAL right),
                                //   EAGAIN (no receiver while a KOS_CAP_HANDOUT holder
                                //   remains, or a far endpoint's peer ring is full),
                                //   ECONNREFUSED (no receiver and no such holder), either
                                //   at once or on waking when the last receiver leaves;
                                //   nothing sent. Parks indefinitely otherwise, and on the
                                //   published console while its task lives, receiver or not,
                                //   where a task that set O_NONBLOCK is answered ETIMEDOUT
                                //   instead. EFAULT also answers a rendezvous copy refused at
                                //   either end (sys.h)
    KOS_SYS_NOTIFY_BIND = 28,   // (notify_cap) -> 0, or -KOS_E*: EBADF, EACCES (cap lacks
                                //   WAIT), EBUSY (another thread is bound, or the caller is
                                //   already bound to a different object), EOVERFLOW (the
                                //   object's reference count is at its ceiling)
    KOS_SYS_CONSOLE_PUBLISH = 29, // (endpoint_cap, task) -> 0, -KOS_EPERM (no
                                  //   KOS_AUTH_CONSOLE, or a task the caller did not create),
                                  //   -KOS_EBADF (bad cap or task), -KOS_EBUSY (an ended task,
                                  //   or the console's registers held outside that task),
                                  //   -KOS_EACCES (cap lacks HANDOUT), -KOS_EOVERFLOW (a
                                  //   reference or receiver count at its ceiling); seats WAIT
                                  //   on a cap without it. KOS_TASK_NONE: the caller's task
    KOS_SYS_CPU_CLOCK_SET = 30,  // (kos_pstate_t as u32) -> landed core Hz (u64); 0 == cannot-change
    KOS_SYS_GRANT_PROBE = 31,    // (op, base, size) -> Rule 7 grant predicate 0/1, or for ops 6/7
                                 //   the raw reserved-block base/size; a BAD op returns -KOS_EINVAL,
                                 //   the arena scribble without KOS_AUTH_MEMORY -KOS_EPERM
                                 //   (self-test only; compiled out unless KICKOS_HAVE_MPU, and
                                 //   answering the arena scribble alone on a region board
                                 //   without it)
    KOS_SYS_PERIPH_CLOCK_HZ = 32, // (base) -> peripheral branch clock in Hz (u32), 0 if unknown (NO KOS_E*)
    KOS_SYS_PINMUX_SET = 33,  // (port, pin, func) -> 0, -KOS_EPERM (no KOS_AUTH_PINMUX), -KOS_EINVAL (range), -KOS_EBUSY (kernel-owned pin), -KOS_ENOSYS (no backend)
    KOS_SYS_CALL = 34,        // (ep_cap, buf, send_len, recv_cap) -> reply bytes (>= 0), or -KOS_E* (EINVAL/EFAULT/EBADF/EACCES/ENOTSUP,
                              //   EAGAIN no receiver while a KOS_CAP_HANDOUT holder remains,
                              //   or a far endpoint's peer ring is full, and nothing was sent,
                              //   ECONNREFUSED no receiver and no such holder, EPIPE the
                              //   server died holding the request, EMFILE the SERVER's cap
                              //   table has no slot for the reply cap)
    KOS_SYS_REPLY = 35,       // (reply_cap, buf, len) -> 0, or -KOS_E* (EBADF bad/non-reply cap, ESRCH stale caller, EFAULT bad buffer)
    KOS_SYS_SHUTDOWN = 36,    // (status) -> does not return; -KOS_EPERM if refused
    KOS_SYS_MEM_SELF_GRANT = 37, // (base, size, kos_mem_flags) -> 0, or -KOS_E*
                              //   (EPERM/EINVAL/ENOMEM/ENOTSUP/EBUSY). ENOTSUP is a memory
                              //   type this chip cannot honour; EINVAL an undefined flag bit,
                              //   or a range overlapping the caller's own regions in a way its
                              //   MPU cannot decide (any overlap on ARMv8-M); EBUSY the range
                              //   mapped elsewhere with another type.
    KOS_SYS_REBOOT = 38,      // () -> does not return; -KOS_EPERM if refused, -KOS_ENOSYS (no backend)
                              //   (self-test only: the dispatch arm is compiled out unless
                              //   KICKOS_ENABLE_SELFTEST, so a production image returns -KOS_EINVAL)
    KOS_SYS_PERIPH_ENABLE = 39, // (base) -> 0, -KOS_EPERM (caller holds no window at that base),
                                //   -KOS_EINVAL (no table entry), -KOS_EIO (the block never left
                                //   reset), -KOS_ENOSYS (no backend).
                                //   Gated on possession, not on an authority bit.
    KOS_SYS_CAP_NARROW = 40,   // (cap, mask) -> 0, -KOS_EBADF (the caller holds no authority
                               //   to give up, or the cap names nothing). Intersects the
                               //   authority word, or any cap's rights. UNGATED by authority.
    KOS_SYS_PANIC = 41,        // (msg) -> does not return. UNGATED by authority. msg is
                               //   copied into kernel memory bounded + byte-checked; a
                               //   message the kernel cannot read is replaced, never
                               //   dereferenced.
    KOS_SYS_PERIPH_REG_WRITE = 42, // (base, offset, value) -> 0, -KOS_EPERM (caller holds no
                               //   window at that base), -KOS_EINVAL (base+offset is not on
                               //   this chip's allowlist), -KOS_ENOSYS (no backend). Gated on
                               //   possession of the block at `base`, not on an authority bit.
    KOS_SYS_NOTIFY = 43,       // (notify_cap) -> 0, -KOS_EALREADY (this capability's badge bit
                               //    was already set and the raise had no effect), or
                               //    -KOS_EBADF/-KOS_EACCES (missing SIGNAL). Raises the bit
                               //    without touching any controller.
    KOS_SYS_IRQ_DISCARD = 44,  // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EACCES (cap lacks WAIT).
                               //   Drops whatever the controller has latched for the line.
                               //   Neither masks nor unmasks.
    KOS_SYS_THREAD_KILL = 45,  // (kos_thread_t) -> 0, -KOS_EBADF (bad/stale/exited handle),
                               //   -KOS_EPERM (the caller did not spawn that thread),
                               //   -KOS_EINVAL (naming yourself; that is KOS_SYS_EXIT).
                               //   COOPERATIVE: the target is woken with -KOS_ECANCELED and
                               //   exits itself.
    KOS_SYS_CALL_TIMED = 46,   // (ep_cap, buf, kos_call_lens_pack(send_len, recv_cap),
                               //   timeout_us) -> as KOS_SYS_CALL, plus -KOS_ETIMEDOUT
    KOS_SYS_NOTIFY_UNBIND = 47, // (notify_cap) -> 0, or -KOS_E*: EBADF, EACCES (cap lacks
                               //   WAIT), EPERM (the caller is not the bound thread). Unconsumed
                               //   bits are LEFT pending for the next server.
    KOS_SYS_THREAD_JOIN = 48,  // (kos_thread_t, timeout_us) -> 0 (the target is gone,
                               //   INCLUDING a target that had already exited),
                               //   -KOS_ETIMEDOUT, -KOS_ECANCELED (the CALLER was cancelled
                               //   while waiting), -KOS_EBADF (never allocated / reclaimed
                               //   under this handle), -KOS_EPERM (the caller did not spawn
                               //   it), -KOS_EDEADLK (naming yourself).
    KOS_SYS_SEND_TIMED = 50,   // (cap, buf, len, timeout_us) -> as KOS_SYS_SEND, plus
                               //   -KOS_ETIMEDOUT, at once for a timeout of 0, which never
                               //   parks. A non-blocking task keeps a finite timeout on the
                               //   console; KOS_TIMEOUT_NONE is a send of no timeout
    KOS_SYS_TASK_CREATE = 51,  // (mem_base, mem_size, kos_task_t* out, kos_mem_flags) -> 0,
                               //   or -KOS_E*: EPERM (no KOS_AUTH_TASKS, an inadmissible
                               //   shared grant, a range the caller never reserved, or a
                               //   caller no member could name), ENOTSUP (a memory type this
                               //   chip cannot honour), EINVAL (the window wraps, no one
                               //   region descriptor names it, or an undefined flag bit),
                               //   ENOMEM (task or domain pool full, or the new space cannot
                               //   take the range at the caller's address), EFAULT (bad
                               //   out-pointer), EBUSY (the range held elsewhere, another
                               //   task's data or a window, with another memory type). The
                               //   first refusal answers, in the order: the out-pointer, the
                               //   flags, KOS_AUTH_TASKS, the range as KOS_SYS_THREAD_CREATE
                               //   admits one, the pools. The task is EMPTY:
                               //   kos_thread_params::task is what seats members.
    KOS_SYS_TASK_KILL = 52,    // (kos_task_t) -> 0, -KOS_EBADF (never created / freed under
                               //   this handle / an implicit task, which is unnameable),
                               //   -KOS_EPERM (the caller did not create it). Slays every
                               //   live member, and kills a privileged one; the handle names
                               //   nothing afterwards.
    KOS_SYS_THREAD_SLAY = 53,  // (kos_thread_t, timeout_us) -> 0 (GONE: the target is EXITED
                               //    and capability teardown finished), -KOS_ETIMEDOUT (termination
                               //    committed, cleanup pending), -KOS_ECANCELED (caller cancelled;
                               //    target still terminates), -KOS_EBADF, -KOS_EPERM (different parent),
                               //    -KOS_EINVAL (self, idle, or privileged target). No cleanup window.
    KOS_SYS_TASK_SLAY = 54,    // (kos_task_t, timeout_us) -> 0 (the group is EMPTY and its
                               //   slot released), -KOS_ETIMEDOUT, -KOS_ECANCELED, -KOS_EBADF,
                               //   -KOS_EPERM (the caller did not create it), -KOS_EINVAL (the
                               //   caller is itself a member, which would wait on its own
                               //   death).
    KOS_SYS_BENCH = 55,        // (kos_bench_op, a0, a1) -> per-op (see enum kos_bench_op),
                               //   -KOS_EINVAL (bad op, bad line, no span open, or an
                               //   argument outside what the op admits), -KOS_ENOSYS (an op
                               //   this image's core count leaves nothing to measure),
                               //   -KOS_EAGAIN (the end-to-end waiter is not parked yet),
                               //   -KOS_EBUSY (the span that closed was not a wake), -KOS_EBADF
                               //   (the arm's handle names no live IRQ cap), -KOS_EACCES (an
                               //   arm whose cap carries no KOS_CAP_WAIT) or -KOS_EPERM (an op
                               //   that reaches the controller or the doorbell issued without
                               //   KOS_AUTH_IRQ, or
                               //   a thread that is not the armed waiter closing or taring a
                               //   span). The counted ops refuse a count above
                               //   KOS_BENCH_SAMPLES_MAX / KOS_BENCH_ROUNDS_MAX. The
                               //   dispatch arm is compiled out unless KICKOS_BENCH, so a
                               //   normal image returns -KOS_EINVAL for every op. THE RESULT
                               //   IS READ AS A SIGNED 64-BIT WORD.
    KOS_SYS_CALL_REG = 56,     // (ep_cap, kos_call_lens_pack(send_len, recv_cap), payload in
                               //   the remaining argument registers) -> as KOS_SYS_CALL, with
                               //   the reply delivered in registers too. INTERNAL:
                               //   kos_call selects it on size alone. Implemented
                               //   ONLY in the trap-handler fastpath; the generic dispatch
                               //   answers KOS_CALL_REG_FALLBACK, the stub's cue to re-issue
                               //   as KOS_SYS_CALL.
    KOS_SYS_IPC_FAST_TAKEN = 57, // ()  -> count of calls the trap-handler IPC fastpath
                               //   COMPLETED (self-test only). Reads 0 on a backend whose
                               //   calls all take the generic path.
    KOS_SYS_NEST_WITNESS = 58, // (which) -> one nested-trap counter (self-test only), or
                               //   KOS_NEST_UNSET for a figure nothing recorded.
    KOS_SYS_ASPACE_PROBE = 59, // (op, a1) -> per-op (see enum kos_aspace_op), or -KOS_EINVAL
                               //   for a bad op, -KOS_EPERM for an op that mints from a caller
                               //   the mint gate refuses, and -KOS_ENOSYS on a board that
                               //   describes regions instead of translating (self-test only). A
                               //   PRODUCTION image answers -KOS_EINVAL from the unknown-number
                               //   arm, so a caller reading the refusal to learn whether the
                               //   board translates must tell ENOSYS from EINVAL.
    KOS_SYS_FRAME_MAP = 60,    // (frame cap, address-space cap, virtual address, KOS_MEM_*)
                               //   -> 0, or -KOS_EPERM without AUTH_MEMORY, -KOS_EBADF on a
                               //   cap that does not resolve, -KOS_EINVAL on a misaligned
                               //   address, -KOS_ENOMEM when the space cannot take the range
                               //   there, -KOS_EBUSY while another mapping of the run carries
                               //   another memory type. The ADDRESS is an argument and never a
                               //   struct field.
    KOS_SYS_FRAME_UNMAP = 61,  // (frame cap, address-space cap, virtual address) -> 0, or
                               //    -KOS_EBADF for an invalid cap, -KOS_EINVAL for a nontranslating space,
                               //    -KOS_EPERM for a range not mapped through KOS_SYS_FRAME_MAP.
                               //    Receivers using the unmapped range are woken with -KOS_EFAULT.
    KOS_SYS_AMP_ENDPOINT_CREATE = 62, // (node, port, kos_cap_t* out) -> 0, or -KOS_ENOSYS on an
                               //   image running one kernel, -KOS_EPERM for an unprivileged
                               //   caller, -KOS_EINVAL for the caller's own node or a port
                               //   that node did not mint, -KOS_EFAULT for an out-pointer the
                               //   caller does not own, -KOS_ENOMEM (endpoint pool) and
                               //   -KOS_EMFILE (the caller's cap table). The cap it grants is
                               //   SIGNAL-only.
    KOS_SYS_THREAD_SET_AFFINITY = 63, // (kos_thread_t, core mask) -> 0, or -KOS_EPERM
                               //   (no allowed core or unauthorized cross-task access), -KOS_EINVAL
                               //   (nonzero mask with no machine core), -KOS_EBADF, -KOS_ENOSYS
                               //   (single-core kernel). Zero selects task defaults; a nonzero mask
                               //   intersects the machine and task grant and may include isolated cores.
    KOS_SYS_TASK_SCHED_GRANT = 64, // (kos_task_t, priority ceiling, core mask) -> 0, or
                               //   -KOS_EPERM (either half wider than the creator's own
                               //   grant, or a caller that did not create the task),
                               //   -KOS_EINVAL (a mask naming no core this kernel schedules,
                               //   or a ceiling outside the priority range), -KOS_EBADF,
                               //   -KOS_EBUSY (the task already has a member).
                               //   NARROWING-ONLY, and 0 in either field leaves that half
                               //   alone.
    KOS_SYS_SCHED_PROBE = 65,  // (op) -> per-op (see enum kos_sched_op), or -KOS_EINVAL for a
                               //   bad op (self-test only: the dispatch arm is compiled out
                               //   unless KICKOS_ENABLE_SELFTEST AND the kernel drives more
                               //   than one core, so every other image returns -KOS_EINVAL
                               //   for every op).
    KOS_SYS_AMP_PROBE = 66,    // (op, a1) -> per-op (see enum kos_amp_op), or -KOS_EINVAL for a
                               //   bad op, -KOS_EPERM on an op that publishes into a peer from
                               //   an unprivileged caller, -KOS_ENOSYS where the backend seats
                               //   no IPI (self-test only: the dispatch arm is compiled out
                               //   unless KICKOS_ENABLE_SELFTEST AND the image is a node of a
                               //   partition, so every other image returns -KOS_EINVAL for
                               //   every op).
    KOS_SYS_DOORBELL_PROBE = 67, // (op, a1) -> per-op (see enum kos_doorbell_op), or
                               //    -KOS_EINVAL for a bad op (self-test only). Returns zero when
                               //    doorbells are compiled out.
    KOS_SYS_REPLY_RECV = 68,   // (reply_cap, buf, kos_call_lens_pack(reply_len, recv_cap),
                               //   kos_reply_recv_opts* in-out) -> received bytes, or
                               //   -KOS_ENOTIFY (a notification and no message), -KOS_EINVAL,
                               //   -KOS_EFAULT, -KOS_EPERM, -KOS_EACCES, -KOS_EBADF,
                               //   -KOS_ESRCH, -KOS_EMFILE, -KOS_ENOTSUP, -KOS_EPIPE,
                               //   -KOS_ETIMEDOUT, -KOS_ECANCELED
    KOS_SYS_NOTIFY_BADGE = 69, // (source notify_cap, bit, kos_cap_t* out) -> 0, or -KOS_E*:
                               //   EBADF, EINVAL (bit at or above 32, or a bad out-ptr),
                               //   EFAULT (out-ptr), EACCES (a badged source cannot mint),
                               //   EMFILE (caller's cap table), EOVERFLOW (the object's
                               //   reference count). MINTS a second name for the same object,
                               //   badged with `bit` and carrying the source's rights.
    KOS_SYS_IRQ_BIND_NOTIFY = 70, // (irq_cap, notify_cap) -> 0, or -KOS_E*: EBADF, EACCES (the
                               //   line's cap lacks WAIT, or the notification's lacks SIGNAL),
                               //   EPERM (the notification carries a line claimed on another
                               //   core), EBUSY (this line already signals something), EAGAIN
                               //   (a holding task's notification budget), EOVERFLOW (the
                               //   object's reference count is at its ceiling). ONE-WAY:
                               //   nothing detaches a live binding.
    KOS_SYS_THREAD_SELF = 71,  // () -> the caller's own kos_thread_t, zero-extended to 64 bits,
                               //   or -KOS_EINVAL to a caller no pool slot seats.
    KOS_SYS_TASK_WATCH = 72,   // (kos_task_t, notify_cap, ready_ep) -> 0, or -KOS_E*: EBADF
                               //   (stale task, or a cap naming nothing of its kind), EPERM
                               //   (not the creator), EACCES (notify cap lacks SIGNAL).
                               //   Raises the notify cap's badge when the task ends, when it
                               //   is dead, its members' teardown done, and the first time a
                               //   member waits on ready_ep. KOS_CAP_NONE for notify_cap
                               //   disarms.
    KOS_SYS_TASK_STATE = 73,   // (kos_task_t) -> enum kos_task_state bits, or -KOS_EBADF
                               //   (stale task), -KOS_EPERM (not the creator).
    KOS_SYS_WINDOW_GET = 74,   // (index, struct kos_window* out) -> 0 with *out the index-th
                               //   window of the caller's spawn list, its base where the caller
                               //   reaches it, or -KOS_EINVAL (index past the list, or out),
                               //   -KOS_EFAULT (out).
    KOS_SYS_PORT_REG_WRITE = 75, // (base, offset, value) -> 0, or -KOS_EPERM (no port window of
                               //   the caller's covers base + offset), -KOS_EINVAL (a port the
                               //   chip does not keep for this write, a value outside its mask,
                               //   or past port 0xffff / a byte), -KOS_ENOSYS (an arch with no
                               //   ports).
    KOS_SYS_TASK_EXIT_STATUS = 76, // (kos_task_t, int* status) -> 0 with *status the status the
                               //   task ended with, or -KOS_EBUSY (not ended yet),
                               //   -KOS_EBADF (stale task), -KOS_EPERM (not the creator),
                               //   -KOS_EINVAL / -KOS_EFAULT (status).
    KOS_SYS_THREAD_SET_PRIORITY = 77, // (priority) -> 0, or -KOS_EINVAL (outside
                               //   KICKOS_PRIO_MIN to KICKOS_PRIO_MAX), -KOS_EPERM (above the
                               //   calling task's priority ceiling). The caller's OWN base
                               //   priority; an inherited boost above it stays.
    KOS_SYS_TASK_NONBLOCK = 78 // (enum kos_nonblock_op) -> the calling task's O_NONBLOCK as
                               //   the op leaves it, 0 or 1, or -KOS_EINVAL (an unknown op, or
                               //   a caller in no task). One flag per task, shared by its threads
};
// The raw console write's number, refused in an app as kos_kconsole_write is (<kickos/sys.h>).
#if defined(main)
#pragma GCC poison KOS_SYS_KCONSOLE_WRITE
#endif

// KOS_SYS_TASK_NONBLOCK's ops on the calling task's O_NONBLOCK, which applies to its sends of no
// timeout on the published console and its kernel console writes.
enum kos_nonblock_op
{
    KOS_NONBLOCK_GET = 0,
    KOS_NONBLOCK_SET = 1,
    KOS_NONBLOCK_CLEAR = 2
};

// What KOS_SYS_TASK_STATE answers for the task a handle names.
enum kos_task_state
{
    KOS_TASK_LIVE = 1 << 0,  // it has members
    KOS_TASK_READY = 1 << 1, // a member has waited on the watched endpoint since it last emptied
    KOS_TASK_DEAD = 1 << 2,  // it is empty and every member's teardown is done: its death
    KOS_TASK_ENDED = 1 << 3  // its status is set and it takes no member; set by its death too
};

/* Slots in ONE ring of an ordered pair. The reply-record band the thread pool reserves is sized
   by it, so a second spelling would size that band against a ring the window does not have. */
#define KOS_AMP_RING_SLOTS 4

/* The window layer's own two ports, which every node mints and no node binds. Every OTHER
   port is the partition's: CONFIG_KICKOS_AMP_PORTS names which node serves which, and
   <kickos/amp.h> is how an app names the capability it was handed for one. */
enum
{
    KOS_AMP_PORT_ECHO = 0, /* the payload comes back to the sender's reply port */
    KOS_AMP_PORT_REPLY = 1 /* a reply, routed to whatever local caller its tag names */
};

// Selectors for KOS_SYS_NEST_WITNESS. NEST_ROOM is the bytes between the lowest nested frame
// seen on a thread stack and that stack's base; compare it against the arch's own interrupt
// red zone, since less than that means the ISR below the frame had no bound.
#define KOS_NEST_TRAPS   0
#define KOS_NEST_ONSTACK 1
#define KOS_NEST_ROOM    2
#define KOS_NEST_UNSET   0xFFFFFFFFu

// The generic dispatch's answer for KOS_SYS_CALL_REG: retry through KOS_SYS_CALL. Outside the
// result range by construction, so it can never collide with a real answer.
#define KOS_CALL_REG_FALLBACK ((int32_t)0x80000000)

// Flags for KOS_SYS_IRQ_CLAIM. The trigger type is fixed at claim time and never changes for
// the binding's life.
enum kos_irq_claim_flags
{
    KOS_IRQ_EDGE = 0,      // default: latch-and-coalesce rearm (bare unmask)
    KOS_IRQ_LEVEL = 1 << 0 // rearm discards the latch first, then unmasks
};

// `flags` for the two calls that create a MAPPING of an arena block: KOS_SYS_MEM_SELF_GRANT
// and KOS_SYS_TASK_CREATE. They select the memory TYPE the region is committed with; access
// is always read-write and is not expressible here. An undefined bit is -KOS_EINVAL, never
// masked off.
// The flag belongs to the BLOCK: pass it identically to EVERY call that maps it, or two live
// mappings end up disagreeing about its type. A grant carried by a SPAWN has no field for it
// here, so that mapping is always Normal and a block reaching a task that way cannot be given
// another type at all.
enum kos_mem_flags
{
    // Map the block Normal non-cacheable, for a block a bus master reads or writes. A chip whose
    // region descriptors carry no memory type and whose data cache sits over the arena REFUSES it
    // with -KOS_ENOTSUP; a chip with no cache in that path accepts it; a chip that TRANSLATES
    // answers from its page tables. The kernel's own copies into and out of the block, IPC
    // included, maintain the data cache around themselves, as a translating chip's mapping does,
    // so no line of the kernel's cacheable view of it is read in place of memory or evicted over
    // it.
    KOS_MEM_NOCACHE = 1u << 0
};
#define KOS_MEM_FLAGS_ALL (KOS_MEM_NOCACHE)

// KOS_SYS_BENCH operation IDs (benchmark images only). Append new values.
// KOS_BENCH_OP_DIST_PRINT's rows: the switch row alone, its window run with every vector register
// written before each switch.
#define KOS_BENCH_DIST_SWITCH_VEC 1

// Unknown operations return -KOS_EINVAL. PRINT operations use the kernel console.
// Operations marked AUTH_IRQ require KOS_AUTH_IRQ or return -KOS_EPERM.
enum kos_bench_op
{
    KOS_BENCH_OP_RESET = 0,       // ()          -> 0. Every distribution AND every phase.
    KOS_BENCH_OP_CYCCNT_HZ = 1,   // ()          -> rate of the counter the cycle ops read,
                                  //   which is not always the core clock, as a 64-BIT Hz
                                  //   count. 0 = no rate converts a reading, so report
                                  //   cycles alone.
    KOS_BENCH_OP_DIST_PRINT = 2,  // (rows)      -> switch sample count (the kernel prints one
                                  //   line per workload-fed distribution; a SWEPT one is
                                  //   printed by the op that filled it). rows
                                  //   KOS_BENCH_DIST_SWITCH_VEC prints the switch row alone,
                                  //   labelled as the dirty-vector variant; any other non-zero
                                  //   rows is refused.
    KOS_BENCH_OP_IRQ_SETUP = 3,   // (line)      -> 0, or -KOS_EBUSY if irq_attach refuses the
                                  //   line. AUTH_IRQ. Names the line every IRQ op below uses.
    KOS_BENCH_OP_IRQ_SWEEP = 4,   // (samples)   -> samples taken (the kernel prints the
                                  //   inject->entry row). 0 = the controller never raised it.
                                  //   AUTH_IRQ; samples above KOS_BENCH_SAMPLES_MAX refused.
    KOS_BENCH_OP_IRQ_WCASE = 5,   // (samples)   -> samples taken over every masked span, each
                                  //   span sampled in turn per round (the kernel prints one
                                  //   worst-case row per span). AUTH_IRQ; samples above
                                  //   KOS_BENCH_SAMPLES_MAX refused.
    KOS_BENCH_OP_PHASE_PRINT = 6, // ()          -> 0 (kernel prints the phase table)
    KOS_BENCH_OP_LOCK_PROBE = 7,  // ()          -> 0 (kernel prints one line: how far the
                                  //   lock distributions moved across three nested IrqLocks,
                                  //   and on which core)
    KOS_BENCH_OP_DOORBELL_PROBE = 9, // (core, rounds) -> rounds run, 0 where the calling
                                  //   thread may not run on that core, -KOS_EINVAL for a core
                                  //   this kernel does not schedule, which is how a caller
                                  //   walks the cores without being told how many there are.
                                  //   -KOS_ENOSYS at one kernel core. The kernel prints one
                                  //   line. AUTH_IRQ; rounds above KOS_BENCH_ROUNDS_MAX
                                  //   refused, the rounds running under one IrqLock.
    // The end-to-end span, raise to the woken userspace thread's first device read. ARM,
    // TARE and CLOSE are the WAITER's; the kernel refuses a close from any other thread.
    // RAISE injects on the line the ARM named and takes no authority of its own.
    KOS_BENCH_OP_E2E_ARM = 10,    // (irq_cap)   -> 0, -KOS_EBADF (no such cap) or -KOS_EACCES
                                  //   (the cap carries no KOS_CAP_WAIT). The span's line is
                                  //   the one that cap names.
    KOS_BENCH_OP_E2E_RAISE = 11,  // ()          -> 0, or -KOS_EAGAIN until the waiter has
                                  //   published its own park, which the caller retries.
    KOS_BENCH_OP_E2E_TARE = 12,   // ()          -> 0. Opens a span with no raise, so the
                                  //   close prices the instrument's own tail.
    KOS_BENCH_OP_E2E_CLOSE = 13,  // ()          -> 0, or -KOS_E* for a span that was not a
                                  //   wake (counted as dropped and reported)
    KOS_BENCH_OP_E2E_PRINT = 14,  // (asked)     -> 0 (kernel prints the probe line, the pass
                                  //   denominator and the two locality rows). `asked` is the
                                  //   sweep size the CALLER ran; the kernel echoes it beside
                                  //   the raises it let through and acts on it in no other
                                  //   way.
    KOS_BENCH_OP_SCHED_PRINT = 15, // (tag)      -> 0 (kernel prints the scheduler's report
                                  //   since the last reset, labelled with `tag`). -KOS_ENOSYS
                                  //   at one kernel core and in an image built without
                                  //   KICKOS_BENCH_SCHED, neither of which keeps the report.
    KOS_BENCH_OP_E2E_QUIET = 16,  // ()          -> 1 once every other kernel core runs its idle
                                  //   thread and no thread is on its way to any core, else 0.
                                  //   The raiser polls it before a pass's first raise.
    KOS_BENCH_OP_FASTPATH_WATCH = 17 // ()       -> 0 (kernel prints one line: the register-form
                                  //   call's taken fastpath entries since boot whose saved or
                                  //   restored DPFPU bank stood at 4 mod 8). -KOS_ENOSYS where
                                  //   no backend keeps that count, which is every one but RXv3.
};

// The largest count the counted ops admit. Raising either raises what a caller can make the
// kernel do in one syscall, the rounds under an IrqLock.
#define KOS_BENCH_SAMPLES_MAX 100u
#define KOS_BENCH_ROUNDS_MAX 64u

// What kos_recv_info.badge carries for a sender holding no badge. Distinct from a zero
// badge_out, which means the receiver asked for no info.
#define KOS_BADGE_NONE 0u

// Receive metadata: 8 bytes, aligned to 4. Plain sends have KOS_CAP_NONE;
// calls supply a one-shot reply cap that the receiver must reply to or close.
// Receiving without metadata rejects calls with -KOS_ENOTSUP.
struct kos_recv_info
{
    uint32_t badge;      // sender badge, KOS_BADGE_NONE in this stage
    kos_cap_t reply_cap; // KOS_CAP_NONE for a plain send; else a one-shot CAP_REPLY handle
                         // in the receiver's table. Test it against KOS_CAP_NONE: a live
                         // reply cap can have bit 31 set, so no sign test works.
};
KOS_STATIC_ASSERT(sizeof(struct kos_recv_info) == 8, "kos_recv_info must stay 8 bytes (ABI)");

// Receive flags; unknown bits return -KOS_EINVAL.
// KOS_RECV_NO_INFO leaves info untouched and rejects calls with -KOS_ENOTSUP.
#define KOS_RECV_NO_INFO 0x1u

// Arguments for KOS_SYS_REPLY_RECV. ep selects the receive endpoint.
// notify supplies the accepted badge bits on input and consumed bits on output, over
// whatever notification the caller is bound to. Zero accepts none; unaccepted bits remain
// pending. Restore the accepted mask before each call because the kernel overwrites it.
struct kos_reply_recv_opts
{
    kos_cap_t ep;              // IN: the endpoint to receive on (needs CAP_WAIT)
    uint32_t flags;            // IN: KOS_RECV_NO_INFO, or 0
    uint32_t timeout_us;       // IN: relative microseconds, or KOS_TIMEOUT_NONE
    uint32_t notify;           // IN: accepted badge bits; zero accepts none
                               // OUT: consumed bits; unaccepted bits remain pending
    struct kos_recv_info info; // OUT: the arrival, whole-struct
};
KOS_STATIC_ASSERT(sizeof(struct kos_reply_recv_opts) == 24,
                  "kos_reply_recv_opts must stay 24 bytes (ABI)");
KOS_STATIC_ASSERT(offsetof(struct kos_reply_recv_opts, info) == 16,
                  "the nested kos_recv_info must sit at offset 16 (ABI)");

// Initialize receive options and clear outputs. No IRQ notifications are accepted
// by default; set notify before each call that should accept them.
static inline void kos_reply_recv_opts_init(struct kos_reply_recv_opts* o, kos_cap_t ep,
                                            uint32_t flags, uint32_t timeout_us)
{
    o->ep = ep;
    o->flags = flags;
    o->timeout_us = timeout_us;
    o->notify = 0u;
    o->info.badge = 0u;
    o->info.reply_cap = KOS_CAP_NONE;
}

// P-state selector for KOS_SYS_CPU_CLOCK_SET, NOT a raw Hz: the landed Hz is the syscall's
// return value. Carried as a plain u32 in the syscall register, so the width is the stable
// ABI: append new states, never reorder.
enum kos_pstate_e
{
    KOS_PSTATE_MAX = 0, // full PLL (the boot clock: XMC 144 / K64F 120 MHz)
    KOS_PSTATE_MID,     // a reduced locked-PLL / staged point (chip rounds to nearest)
    KOS_PSTATE_LOW      // deep power saving (crystal/RC direct or a low staged point)
};
// The ABI type is the u32 and not the enum: an enum's own width is implementation-defined before
// C23 (arm-none-eabi short-enums this one to a single byte; GNURX does not), and a fixed
// underlying type is not ISO C11. A caller wanting -Wswitch exhaustiveness back switches on
// `(enum kos_pstate_e)x`.
typedef uint32_t kos_pstate_t;

// Shared payload bound: send REJECTS a len above this; recv clamps its capacity to it.
#define KOS_EP_MSG_MAX 256

// A timeout is RELATIVE microseconds; KOS_TIMEOUT_NONE means no deadline. The fleet-wide
// timer floor is 20 us.
#define KOS_TIMEOUT_NONE UINT32_MAX

// KOS_SYS_CALL_TIMED packs both message lengths into ONE argument slot so the fourth can
// carry the deadline. Nine bits each, both being bounded by KOS_EP_MSG_MAX.
#define KOS_CALL_LEN_BITS 9
#define KOS_CALL_LEN_MASK ((1u << KOS_CALL_LEN_BITS) - 1u)
KOS_STATIC_ASSERT((unsigned)KOS_EP_MSG_MAX < KOS_CALL_LEN_MASK,
                  "KOS_EP_MSG_MAX must stay strictly below the packed field's saturation value");

// Saturates at the field width: a masked 512 would arrive as 0 and become a silent zero-length
// call, while a saturated 511 still trips the kernel's send_len > KOS_EP_MSG_MAX refusal.
static inline uintptr_t kos_call_len_field(size_t len)
{
    if (len > (size_t)KOS_CALL_LEN_MASK)
    {
        return (uintptr_t)KOS_CALL_LEN_MASK;
    }
    return (uintptr_t)len;
}
static inline uintptr_t kos_call_lens_pack(size_t send_len, size_t recv_cap)
{
    return (kos_call_len_field(recv_cap) << KOS_CALL_LEN_BITS) | kos_call_len_field(send_len);
}
static inline size_t kos_call_lens_send(uintptr_t packed)
{
    return (size_t)(packed & KOS_CALL_LEN_MASK);
}
static inline size_t kos_call_lens_recv(uintptr_t packed)
{
    return (size_t)((packed >> KOS_CALL_LEN_BITS) & KOS_CALL_LEN_MASK);
}

// KOS_SYS_CALL_REG's payload budget, in 32-bit words and in bytes. Five is what an rv32
// psABI leaves after (nr, ep_cap, lens) claim a0-a2; the reply comes back in a1-a5.
#define KOS_CALL_REG_WORDS 5
#define KOS_CALL_REG_BYTES (KOS_CALL_REG_WORDS * 4)

// Counting-semaphore ceiling. sem_create refuses an initial outside [0, KOS_SEM_COUNT_MAX]
// with -KOS_EINVAL; a post at the ceiling is refused with -KOS_EOVERFLOW.
#define KOS_SEM_COUNT_MAX 0x7FFFFFFF

// 64-bit ARGUMENTS are passed as two uintptr_t halves, identically on 32-bit (ARM M-class)
// and 64-bit (sim) targets: never rely on uintptr_t being 64 bits. sleep_ns takes (lo, hi).
// A 64-bit RESULT comes back whole in the psABI's long-long return register pair, so it
// needs no out-pointer.
static inline uint32_t kos_u64_lo(uint64_t v)
{
    return (uint32_t)(v & 0xffffffffu);
}
static inline uint32_t kos_u64_hi(uint64_t v)
{
    return (uint32_t)(v >> 32);
}
static inline uint64_t kos_u64_join(uint32_t lo, uint32_t hi)
{
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

enum kos_policy
{
    KOS_POLICY_FIFO = 0,
    KOS_POLICY_RR = 1
};

// One entry of a spawn delegation list: hand the child a narrowed copy of the parent cap
// `source_cap`. A fresh child table has cap-gen 0, so the child's handle value EQUALS its
// index and is known a priori with no handoff.
//
// Table index of the FIRST delegated cap (i == 0) under DEFAULT placement: delegated cap
// i lands at KOS_SPAWN_DELEGATED_CAP0 + i.
#define KOS_SPAWN_DELEGATED_CAP0 1
struct kos_cap_grant
{
    kos_cap_t source_cap; // a cap handle in the SPAWNING thread's table
    uint8_t rights_mask;  // subset of the source cap's rights (kos_cap_rights bits)
};

// What one window of a spawn's list reaches.
enum kos_window_kind
{
    // Device registers, R|W and never executable, with one holder: overlapping a window a
    // live thread holds, or another device entry of the same list, is -KOS_EBUSY. Needs
    // KOS_AUTH_MEMORY, and KOS_AUTH_BUS_MASTER as well over a device its chip file marks
    // `bus_master` (-KOS_EPERM). Takes no flag, and is what kos_periph_enable and
    // kos_periph_reg_write accept.
    KOS_WINDOW_DEVICE = 0,
    // A block the spawner's task reserved with kos_ram_alloc, or any part on whole granules of
    // an AMP partition's user share where the spawner is in root's task (<kickos/amp.h>), at
    // the address kos_window_get answers; a list names a block in one window at most
    // (-KOS_EINVAL). The block keeps one memory type wherever it is mapped: a window asking
    // another is -KOS_EBUSY.
    KOS_WINDOW_MEMORY = 1,
    // An I/O port range, base the first port and size the count, on an arch with ports
    // (-KOS_ENOTSUP elsewhere): inside an aperture the chip states (-KOS_EINVAL), one holder as
    // for a device window, and reached by the holder's own in and out instructions.
    KOS_WINDOW_PORTS = 2
};

// A memory window's flags; a device window takes none (-KOS_EINVAL).
enum kos_window_flags
{
    KOS_WINDOW_RO = 1 << 0,      // reads only: a write faults the thread
    KOS_WINDOW_UNCACHED = 1 << 1 // as KOS_MEM_NOCACHE; -KOS_ENOTSUP where the chip cannot
};

struct kos_window
{
    uintptr_t base;
    uint32_t size;
    uint8_t kind;  // enum kos_window_kind
    uint8_t flags; // enum kos_window_flags
};

// Thread-creation parameters (kernel allocates TCB + stack from a static pool).
struct kos_thread_params
{
    void (*entry)(void* arg);
    void* arg;
    char const* name;
    uint8_t prio;
    uint8_t policy;      // enum kos_policy
    uint8_t privileged;  // 0 => unprivileged user thread
    uint8_t cap_count;   // number of entries in caps[]; under default placement they land
                         // at child indices 1..cap_count.
                         // Above KICKOS_MAX_SPAWN_GRANTS: -KOS_EINVAL. That bound is the
                         // spawn stager's, NOT the child table's ceiling.
    uint32_t quantum_ns; // RR slice; 0 => none
    void* mem_base;      // domain data region granted to the thread (0 => none). A block
                         // kos_ram_alloc handed the CALLER, a sibling task's block included;
                         // the child's task reaches it at the same address.
    uint32_t mem_size;   // size of that region (bytes)
    // The windows this thread holds, each its own and none its task's (0 => none), read
    // once at the spawn. More than KICKOS_MAX_THREAD_WINDOWS entries: -KOS_ENOMEM.
    // A region backend refuses (-KOS_EINVAL) a child whose data, windows and stack overlap in a
    // way its MPU cannot decide as the kernel does: any overlap on ARMv8-M.
    struct kos_window const* windows;
    uint16_t window_count;
    void* stack_base;    // caller-owned thread stack; 0 => kernel default (KICKOS_USER_STACK_SIZE).
                         // Must be a block the CALLER'S TASK reserved with kos_ram_alloc, a
                         // sibling task's included. Under translation it must ALSO be
                         // reachable in the task the child joins. App static data is neither.
                         // -KOS_EINVAL unless 16-aligned; where the thread pointer is SP masked
                         // (armv6m, armv7m, rxv3) and the image carves TLS, also unless it is
                         // KICKOS_TLS_STRIDE-aligned and stack_size is exactly one stride.
    uint32_t stack_size; // size of the caller stack (bytes); ignored when stack_base == 0.
                         // -KOS_EINVAL unless a multiple of 16 and at least KICKOS_MIN_STACK_SIZE
                         // plus the TLS carve: the image's thread_local block rounded to 16, or
                         // with none the control block alone under KICKOS_REENT_IN_TCB.
    struct kos_cap_grant const* caps; // optional caps to delegate to the child (0 => none)
    // Optional uint16_t-aligned destination indices, parallel to caps.
    // Null or zero entries use default placement (grant i -> index i+1).
    // Index 0 is reserved for stdout. The first default slot aliases KOS_CAP_CLOCK;
    // use an explicit destination to preserve that slot.
    // Reject duplicate destinations or indices outside the child table with EINVAL
    // before creating the thread.
    uint16_t const* cap_dest;
    // Allowed child cores. Zero selects task defaults, excluding isolated cores.
    // Explicit masks intersect the machine and task grant; isolated cores require
    // an explicit bit. No machine core returns EINVAL; no granted core returns EPERM.
    // Ignored by single-core kernels.
    uint32_t core_mask;
    // Authority bits (kos_cap_authority KOS_AUTH_*) to seat as the child's authority word;
    // 0 => none. Only a thread that already holds each bit may pass it: narrows, never
    // widens, like a cap_grant mask. A bit no authority names: -KOS_EINVAL.
    uint32_t authority;
    // The task the child JOINS, from kos_task_create, or KOS_TASK_NONE to make the child a
    // thread of the spawner's task (KOS_TASK_NONE above). Only the task's creator may seat a
    // member. A member shares the task's data region, so it may bring NO mem_base of its own
    // (-KOS_EINVAL) and may not be privileged (-KOS_EINVAL); its windows are per-thread and
    // still its own. A named task is the only way to put a thread in a group of its own, and
    // a fault ends the whole task.
    kos_task_t task;
};

// sizeof is NOT assertable here: three pointers make it width-dependent (64 B on armv7m,
// 112 B on a 64-bit host). What is width-independent is the PACKING, and that is the part a
// new field would move silently: the four bytes behind `name` are one full word, and
// core_mask and authority are the two 32-bit words the tail carries with no padding between.
// The spawn stager copies the whole struct onto the syscall stack, so a padding byte here
// is a byte of every trap red zone.
KOS_STATIC_ASSERT(sizeof(((struct kos_thread_params*)0)->core_mask) == 4,
                  "the core mask is a 32-bit word at every ABI that carries one");
KOS_STATIC_ASSERT(sizeof(((struct kos_thread_params*)0)->authority) == 4,
                  "the authority word is 32 bits at every ABI");
KOS_STATIC_ASSERT(offsetof(struct kos_thread_params, authority)
                      == offsetof(struct kos_thread_params, core_mask) + 4,
                  "authority must sit immediately behind core_mask (ABI)");
KOS_STATIC_ASSERT(offsetof(struct kos_thread_params, cap_count)
                      == offsetof(struct kos_thread_params, privileged) + 1,
                  "cap_count must fill the byte behind privileged (ABI)");
KOS_STATIC_ASSERT(offsetof(struct kos_thread_params, quantum_ns)
                      == offsetof(struct kos_thread_params, prio) + 4,
                  "prio, policy, privileged and cap_count must fill one word (ABI)");

#endif
