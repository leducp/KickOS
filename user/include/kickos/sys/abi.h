// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS syscall ABI shared by userspace stubs and the kernel dispatch table. Numbers
// are stable contract; argument packing is uintptr_t-wide.

#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/cap_index.h> // KOS_CAP_AUTHORITY, the well-known indices
#include <kickos/sys/errno.h> // KOS_E* taxonomy: failures return -KOS_Exxx (see below)

// Return-encoding contract (see errno.h). A syscall that can fail returns its error as
// -KOS_Exxx (negative); success is a non-negative byte-count / count, so the two are
// collision-free. Exceptions, all outside the scheme: ram_alloc returns a pointer, 0/NULL on
// ANY failure, and cpu_clock_hz / cpu_clock_set / periph_clock_hz return a u32 Hz with a
// 0 == cannot/unknown sentinel.

// A capability handle. 16 index bits + 16 generation bits, so a live handle spends the
// WHOLE 32-bit word and may have bit 31 set: `h < 0` is not an error test on a capability,
// and every capability-MINTING call returns a status and delivers the handle through an
// out-parameter.
typedef uint32_t kos_cap_t;

// "No capability". No table can mint this word, nor KOS_CAP_AUTHORITY, which shares its
// index field. Written to a minting call's out-parameter on EVERY failure, and carried by
// kos_recv_info.reply_cap for a plain send.
#define KOS_CAP_NONE 0xFFFFFFFFu

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

// The exit code a thread killed by a CPU fault reports: what a joiner reads back, and the
// process status when it was the last thread live. A clean kos_exit(139) aliases it.
#define KOS_EXIT_FAULT 139

// The exit code a CANCELLED thread reports: the kernel ends it at a syscall boundary, so it
// never picks a code of its own. 128 + SIGINT, as KOS_EXIT_FAULT is 128 + SIGSEGV.
#define KOS_EXIT_CANCELLED 130

enum kos_syscall_nr
{
    KOS_SYS_KCONSOLE_WRITE = 1, // (buf, len)            -> bytes written, or -KOS_EFAULT (bad buffer)
    KOS_SYS_YIELD = 2,          // ()                    -> 0
    KOS_SYS_SLEEP_NS = 3,       // (ns_lo, ns_hi)        -> 0
    KOS_SYS_SEM_CREATE = 4,     // (initial, kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM sem pool,
                                //   EMFILE caller's cap table, EINVAL/EFAULT)
    KOS_SYS_SEM_WAIT = 5,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EPERM
    KOS_SYS_SEM_POST = 6,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EPERM
    KOS_SYS_HANDLE_CLOSE = 17,  // (cap)   -> 0, -KOS_EBADF (bad cap), -KOS_EBUSY (own a held mutex)
    KOS_SYS_THREAD_CREATE = 7,   // (kos_thread_params*, kos_thread_t* out) -> 0, or -KOS_E*
                                //   (EINVAL/EFAULT/EPERM/EBADF/EBUSY/ENOMEM/EOVERFLOW)
    KOS_SYS_EXIT = 8,           // (code)                -> does not return. Ends the calling
                                //   thread, or the SYSTEM when the caller is root, which
                                //   needs KOS_AUTH_SYSTEM for it and panics without.
    KOS_SYS_IRQ_INJECT = 9,     // (irq)                 -> 0, or -KOS_EINVAL (self-test only)
    KOS_SYS_GUARD_ADDR = 10,    // ()  -> protected probe addr (self-test only)
    KOS_SYS_IRQ_ATTACH = 11,    // (irq, sem_handle)  -> 0, or -KOS_E* (EPERM/EINVAL/EBADF/EBUSY)
    KOS_SYS_CLOCK_NOW = 12,     // ()  -> monotonic nanoseconds (u64, in registers; cannot fail)
    KOS_SYS_RAM_ALLOC = 13,     // (size)                -> user-RAM ptr, or 0/NULL on ANY failure
    KOS_SYS_IRQ_CLAIM = 14,     // (line, flags, kos_cap_t* out) -> 0, or -KOS_E*: EPERM (lacks
                                //   KOS_AUTH_IRQ), EINVAL (line/flags/out-ptr), EFAULT (out-ptr),
                                //   EBUSY (line owned), ENOMEM (binding pool), EMFILE (cap table)
    KOS_SYS_IRQ_WAIT = 15,      // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks WAIT)
    KOS_SYS_IRQ_ACK = 16,       // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks WAIT)
    KOS_SYS_IRQ_SPURIOUS = 18,  // ()  -> count of IRQs on unbound lines (self-test only)
    KOS_SYS_DIAG_LED_SET = 19,  // (on)                  -> 0 (kernel diagnostic LED)
    KOS_SYS_DIAG_LED_TOGGLE = 20, // ()                  -> 0 (kernel diagnostic LED)
    KOS_SYS_IRQ_UNMASK = 21,    // (irq)  -> 0, or -KOS_E* (EPERM/EINVAL; self-test only)
    KOS_SYS_CPU_CLOCK_HZ = 22,  // ()  -> running core clock in Hz (u32), 0 if unknown (NO KOS_E*)
    KOS_SYS_MUTEX_CREATE = 23,  // (kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM mutex pool, EMFILE
                                //   caller's cap table, EINVAL/EFAULT)
    KOS_SYS_MUTEX_LOCK = 24,    // (cap)  -> 0 held; -KOS_EOWNERDEAD held-but-owner-died; -KOS_EBADF
                                //   / -KOS_EDEADLK NOT held (see the wrapper decl for the caveat)
    KOS_SYS_MUTEX_UNLOCK = 25,  // (cap)  -> 0, -KOS_EBADF (bad cap), -KOS_EPERM (caller not owner)
    KOS_SYS_ENDPOINT_CREATE = 26, // (kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM endpoint pool,
                                  //   EMFILE caller's cap table, EINVAL/EFAULT)
    KOS_SYS_SEND = 27,          // (cap, buf, len) -> bytes transferred, or -KOS_E*, parking
                                //   indefinitely
    KOS_SYS_RECV = 28,          // (cap, buf, cap_len, kos_recv_info* out) -> bytes received, or -KOS_E*
    KOS_SYS_CONSOLE_PUBLISH = 29, // (endpoint_cap) -> 0, -KOS_EPERM (no KOS_AUTH_CONSOLE),
                                  //   -KOS_EBADF (bad cap), -KOS_EOVERFLOW (endpoint
                                  //   refcount at its ceiling)
    KOS_SYS_CPU_CLOCK_SET = 30,  // (kos_pstate_t as u32) -> landed core Hz (u32); 0 == cannot-change
    KOS_SYS_GRANT_PROBE = 31,    // (op, base, size) -> Rule 7 grant predicate 0/1, or for ops 6/7
                                 //   the raw reserved-block base/size; a BAD op returns -KOS_EINVAL
                                 //   (self-test only; compiled out unless KICKOS_HAVE_MPU)
    KOS_SYS_PERIPH_CLOCK_HZ = 32, // (base) -> peripheral branch clock in Hz (u32), 0 if unknown (NO KOS_E*)
    KOS_SYS_PINMUX_SET = 33,  // (port, pin, func) -> 0, -KOS_EPERM (no KOS_AUTH_PINMUX), -KOS_EINVAL (range), -KOS_EBUSY (kernel-owned pin), -KOS_ENOSYS (no backend)
    KOS_SYS_CALL = 34,        // (ep_cap, buf, send_len, recv_cap) -> reply bytes (>= 0), or -KOS_E* (EINVAL/EFAULT/EBADF/EPERM/EPIPE/ENOSYS,
                              //   EMFILE the SERVER's cap table has no slot for the reply cap)
    KOS_SYS_REPLY = 35,       // (reply_cap, buf, len) -> 0, or -KOS_E* (EBADF bad/non-reply cap, ESRCH stale caller, EFAULT bad buffer)
    KOS_SYS_SHUTDOWN = 36,    // (status) -> does not return; -KOS_EPERM if refused
    KOS_SYS_MEM_SELF_GRANT = 37, // (base, size, kos_mem_flags) -> 0, or -KOS_E*
                              //   (EPERM/EINVAL/ENOMEM). EPERM also covers a memory type
                              //   this chip cannot honour; EINVAL an undefined flag bit.
    KOS_SYS_REBOOT = 38,      // () -> does not return; -KOS_EPERM if refused, -KOS_ENOSYS (no backend)
                              //   (self-test only: the dispatch arm is compiled out unless
                              //   KICKOS_ENABLE_SELFTEST, so a production image returns -KOS_EINVAL)
    KOS_SYS_PERIPH_ENABLE = 39, // (base) -> 0, -KOS_EPERM (caller holds no window at that base),
                                //   -KOS_EINVAL (no table entry), -KOS_ENOSYS (no backend).
                                //   Gated on possession, not on an authority bit.
    KOS_SYS_CAP_NARROW = 40,   // (cap, mask) -> 0, -KOS_EBADF (the caller holds no authority
                               //   to give up), -KOS_EINVAL (not the authority cap).
                               //   UNGATED by authority.
    KOS_SYS_PANIC = 41,        // (msg) -> does not return. UNGATED by authority. msg is
                               //   copied into kernel memory bounded + byte-checked; a
                               //   message the kernel cannot read is replaced, never
                               //   dereferenced.
    KOS_SYS_PERIPH_REG_WRITE = 42, // (base, offset, value) -> 0, -KOS_EPERM (caller holds no
                               //   window at that base), -KOS_EINVAL (base+offset is not on
                               //   this chip's allowlist), -KOS_ENOSYS (no backend). Gated on
                               //   possession of the block at `base`, not on an authority bit.
    KOS_SYS_IRQ_NOTIFY = 43,   // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks SIGNAL).
                               //   Software-posts the binding WITHOUT touching the
                               //   controller.
    KOS_SYS_IRQ_DISCARD = 44,  // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks WAIT).
                               //   Drops whatever the controller has latched for the line.
                               //   Neither masks nor unmasks.
    KOS_SYS_THREAD_KILL = 45,  // (kos_thread_t) -> 0, -KOS_EBADF (bad/stale/exited handle),
                               //   -KOS_EPERM (the caller did not spawn that thread),
                               //   -KOS_EINVAL (naming yourself; that is KOS_SYS_EXIT).
                               //   COOPERATIVE: the target is woken with -KOS_ECANCELED and
                               //   exits itself.
    KOS_SYS_CALL_TIMED = 46,   // (ep_cap, buf, kos_call_lens_pack(send_len, recv_cap),
                               //   timeout_us) -> as KOS_SYS_CALL, plus -KOS_ETIMEDOUT. Both
                               //   lengths share one argument slot so the fourth can carry
                               //   the deadline.
    KOS_SYS_RECV_TIMED = 47,   // (cap, buf, cap_len, kos_recv_timed_opts* in-out) -> as
                               //   KOS_SYS_RECV, plus -KOS_ETIMEDOUT, and -KOS_EINVAL for a
                               //   null opts, which carries the deadline.
    KOS_SYS_THREAD_JOIN = 48,  // (kos_thread_t, timeout_us) -> 0 (the target is gone,
                               //   INCLUDING a target that had already exited),
                               //   -KOS_ETIMEDOUT, -KOS_ECANCELED (the CALLER was cancelled
                               //   while waiting), -KOS_EBADF (never allocated / reclaimed
                               //   under this handle), -KOS_EPERM (the caller did not spawn
                               //   it), -KOS_EDEADLK (naming yourself).
    KOS_SYS_WAIT_LAST = 49,    // () -> 0 once the caller is the last live thread, or
                               //   -KOS_EPERM to any thread but root.
    KOS_SYS_SEND_TIMED = 50,   // (cap, buf, len, timeout_us) -> as KOS_SYS_SEND, plus
                               //   -KOS_ETIMEDOUT
    KOS_SYS_TASK_CREATE = 51,  // (mem_base, mem_size, kos_task_t* out, kos_mem_flags) -> 0,
                               //   or -KOS_E*: EPERM (inadmissible shared grant, a range the
                               //   caller never reserved, a memory type this chip cannot
                               //   honour, or a caller no member could
                               //   name), EINVAL (the window wraps, or an undefined flag
                               //   bit), ENOMEM (task or domain pool full, or the new space
                               //   cannot take the range at the caller's address), EFAULT (bad
                               //   out-pointer). The task is EMPTY:
                               //   kos_thread_params::task is what seats members.
    KOS_SYS_TASK_KILL = 52,    // (kos_task_t) -> 0, -KOS_EBADF (never created / freed under
                               //   this handle / an implicit task, which is unnameable),
                               //   -KOS_EPERM (the caller did not create it). Cancels every
                               //   live member; the handle names nothing afterwards.
    KOS_SYS_THREAD_SLAY = 53,  // (kos_thread_t, timeout_us) -> 0 (GONE: the target is EXITED
                               //   and its capability table is swept), -KOS_ETIMEDOUT (the
                               //   redirect is armed and irrevocable, the target executes no
                               //   further unprivileged instruction, and the sweep has not
                               //   finished), -KOS_ECANCELED (the CALLER was cancelled while
                               //   waiting; the target is still condemned), -KOS_EBADF,
                               //   -KOS_EPERM (the caller did not spawn it), -KOS_EINVAL
                               //   (self, idle, or a privileged target). FORCIBLE, where
                               //   KOS_SYS_THREAD_KILL is cooperative: the target gets no
                               //   cleanup window.
    KOS_SYS_TASK_SLAY = 54,    // (kos_task_t, timeout_us) -> 0 (the group is EMPTY and its
                               //   slot released), -KOS_ETIMEDOUT, -KOS_ECANCELED, -KOS_EBADF,
                               //   -KOS_EPERM (the caller did not create it), -KOS_EINVAL (the
                               //   caller is itself a member, which would wait on its own
                               //   death).
    KOS_SYS_BENCH = 55,        // (kos_bench_op, a0, a1) -> per-op (see enum kos_bench_op),
                               //   or -KOS_EINVAL (bad op). UNGATED by authority; the
                               //   dispatch arm is compiled out unless KICKOS_BENCH, so a
                               //   normal image returns -KOS_EINVAL.
    KOS_SYS_CALL_REG = 56,     // (ep_cap, kos_call_lens_pack(send_len, recv_cap), payload in
                               //   the remaining argument registers) -> as KOS_SYS_CALL, with
                               //   the reply delivered in registers too. INTERNAL:
                               //   kos_call selects it on size alone. Implemented
                               //   ONLY in the trap-handler fastpath; the generic dispatch
                               //   answers KOS_CALL_REG_FALLBACK, the stub's cue to re-issue
                               //   as KOS_SYS_CALL.
    KOS_SYS_IPC_FAST_TAKEN = 57, // ()  -> count of calls the trap-handler IPC fastpath
                               //   COMPLETED (self-test only). The fastpath and the buffer
                               //   form answer a caller identically, so this counter is the
                               //   only thing that separates them. Reads 0 on a backend
                               //   whose calls all take the generic path.
    KOS_SYS_NEST_WITNESS = 58, // (which) -> one nested-trap counter (self-test only), or
                               //   KOS_NEST_UNSET for a figure nothing recorded.
    KOS_SYS_ASPACE_PROBE = 59, // (op, a1) -> per-op (see enum kos_aspace_op), or -KOS_EINVAL
                               //   for a bad op and -KOS_ENOSYS on a board that describes
                               //   regions instead of translating (self-test only).
    KOS_SYS_FRAME_MAP = 60,    // (frame cap, address-space cap, virtual address, KOS_MEM_*)
                               //   -> 0, or -KOS_EPERM without AUTH_MEMORY or on a cap that
                               //   does not resolve, -KOS_EINVAL on a misaligned address,
                               //   -KOS_ENOMEM when the space cannot take the range there.
                               //   The ADDRESS is an argument and never a field: no struct
                               //   here carries one, which is what keeps it out of the
                               //   capability ABI's own records.
    KOS_SYS_FRAME_UNMAP = 61,  // (frame cap, address-space cap, virtual address) -> 0, or
                               //   -KOS_EPERM for a range this space did not take through
                               //   KOS_SYS_FRAME_MAP, which is what stops one holder
                               //   revoking another's mapping.
    KOS_SYS_AMP_ENDPOINT_CREATE = 62, // (node, port, kos_cap_t* out) -> 0, or -KOS_ENOSYS on an
                               //   image running one kernel, -KOS_EPERM for an unprivileged
                               //   caller, -KOS_EINVAL for the caller's own node or a port
                               //   that node did not mint, plus the mint refusals every
                               //   creator carries. The cap it grants is SIGNAL-only.
    KOS_SYS_THREAD_SET_AFFINITY = 63, // (kos_thread_t, core mask) -> 0, or -KOS_EPERM (the
                               //   mask meets the thread's task's core set nowhere, or the
                               //   target is in another task and the caller is unprivileged),
                               //   -KOS_EINVAL (a NONZERO mask naming no core this kernel
                               //   schedules), -KOS_EBADF. The ONE placement operation: pin is
                               //   a mask of one bit and unpin is a mask of ZERO, which the
                               //   kernel resolves to the task's DEFAULT set exactly as a
                               //   spawn's zero core_mask is resolved. A NONZERO mask is a SET
                               //   OF ACCEPTABLE CORES and is intersected with the machine
                               //   before the grant, so a bit naming no core costs nothing
                               //   beside one that does, and a bit naming an isolated core is
                               //   the opt-in that reaches it.
                               //   -KOS_ENOSYS on an image whose kernel drives one core.
    KOS_SYS_TASK_SCHED_GRANT = 64, // (kos_task_t, priority ceiling, core mask) -> 0, or
                               //   -KOS_EPERM (either half wider than the creator's own
                               //   grant, or a caller that did not create the task),
                               //   -KOS_EINVAL (a mask naming no core this kernel schedules,
                               //   or a ceiling outside the priority range), -KOS_EBADF,
                               //   -KOS_EBUSY (the task already has a member).
                               //   NARROWING-ONLY, and 0 in either field leaves that half
                               //   alone.
    KOS_SYS_SCHED_PROBE = 65   // (op) -> per-op (see enum kos_sched_op), or -KOS_EINVAL for a
                               //   bad op (self-test only: the dispatch arm is compiled out
                               //   unless KICKOS_ENABLE_SELFTEST AND the kernel drives more
                               //   than one core, so every other image returns -KOS_EINVAL
                               //   for every op).
};

// `op` selector for KOS_SYS_SCHED_PROBE (self-test only). Values are a frozen contract:
// append, never reorder.
enum kos_sched_op
{
    // The core the caller is running on AT THE MOMENT OF THE READ. Meaningful only for a
    // thread whose affinity is a single bit; anything else may have moved by the time the
    // answer lands, which is the point of asking it of a pinned thread.
    KOS_SCHED_OP_CORE = 0,
    KOS_SCHED_OP_AFFINITY = 1,   // () -> the caller's own core mask
    KOS_SCHED_OP_TASK_CORES = 2, // () -> the caller's task's core set
    KOS_SCHED_OP_CEILING = 3,    // () -> the caller's task's priority ceiling
    KOS_SCHED_OP_ISOLATED = 4    // () -> the cores this image isolates
};

// `op` selector for KOS_SYS_ASPACE_PROBE (self-test only). Values are a frozen contract:
// append, never reorder.
enum kos_aspace_op
{
    KOS_ASPACE_OP_GRANULE = 0,   // () -> the map editor's granule in bytes
    KOS_ASPACE_OP_MEMTYPE = 1,   // (enum arch_map_memtype as a number) -> 1 honoured, 0 not
    KOS_ASPACE_OP_FRAMES_FREE = 2, // () -> frames the kernel's one pool has left
    KOS_ASPACE_OP_ROUNDTRIP = 3, // () -> how far map, write, read back, unmap got (0..4)
    KOS_ASPACE_OP_ALIAS = 4,     // () -> 1 when two unequal virtual pages reached the one
                                 //   frame the caller chose, which an identity map cannot do
    KOS_ASPACE_OP_REFUSALS = 5,  // () -> the KOS_ASPACE_REFUSE_* bits that held
    KOS_ASPACE_OP_BALANCE = 6,   // () -> frames a create/map/unmap/destroy cycle did not
                                 //   return; 0 is balanced, and all ones means the cycle
                                 //   asked the pool to free a frame it does not own
    KOS_ASPACE_OP_TOUCH_UNMAPPED = 7, // () -> activates a space and reads a page it just
                                 //   unmapped, so on a working backend it does not return
    KOS_ASPACE_OP_SPAN = 8,      // () -> 1 when a range crossing two table boundaries mapped
                                 //   contiguously and unmapped whole
    KOS_ASPACE_OP_SPACE_ID = 9,  // () -> a small stable name for the CALLING task's address
                                 //   space, 0 when it holds none. Two tasks answering the
                                 //   same number are one address space; the number is not a
                                 //   kernel address and nothing else may be read out of it
    KOS_ASPACE_OP_DOMAIN_BALANCE = 10, // () -> frames a run of domain resolves and releases
                                 //   did not return; 0 is balanced
    KOS_ASPACE_OP_RANGES_FREE = 11, // () -> range slots the CALLING task's space has left.
                                 //   Allocation spends one and there is no free, so this is
                                 //   how many more blocks the caller may reserve
    KOS_ASPACE_OP_FRAME_AT = 12, // (a virtual address) -> a small stable name for the frame
                                 //   backing it in the CALLING task's space, 0 when the page
                                 //   is not mapped. Two tasks answering DIFFERENT numbers for
                                 //   one address are two copies of it; the number counts frames
                                 //   from the image's first text page, which is no address at
                                 //   all, and nothing else may be read out of it
    KOS_ASPACE_OP_SPLIT_ACCESS = 13, // () -> the KOS_ASPACE_SPLIT_* bits that held for a
                                 //   virtually contiguous range whose two pages are backed by
                                 //   NON-ADJACENT frames, which no caller can itself build
    KOS_ASPACE_OP_FORCED_UNWIND = 14, // (0, or a range this task reserved) -> the
                                 //   KOS_ASPACE_UNWIND_* bits that held, with the number of
                                 //   injection points the sweep reached above bit
                                 //   KOS_ASPACE_UNWIND_DEPTH_SHIFT. A forced allocation failure
                                 //   is walked through the domain-create path one allocation at
                                 //   a time, so every unwind arm under it runs once. At 0 the
                                 //   path is the no-grant create; with a reservation named it is
                                 //   the grant-carrying one, whose handoff unwinds separately
    KOS_ASPACE_OP_SPACES_HELD = 15, // () -> how many domain slots hold an address space at all.
                                 //   Churn that ends with a different answer stranded a root,
                                 //   which a frame delta only implies
    KOS_ASPACE_OP_MODEL = 16,    // () -> what the implementation reports about its own
                                 //   translation, as the KOS_ASPACE_MODEL_* bits and the three
                                 //   widths beside them, which the port's own recorded figures
                                 //   are compared against
    KOS_ASPACE_OP_MEMTYPE_AT = 17, // (a virtual address) -> 1 + the memory TYPE the CALLING
                                 //   task's mapping of it carries (enum arch_map_memtype),
                                 //   or 0 when the page is not mapped. The flag belongs to
                                 //   the block, so two mappings of one block answering
                                 //   differently is the disagreement kos_mem_flags warns of
    KOS_ASPACE_OP_ACQUIRE_BALANCE = 18, // () -> outstanding page acquires in the high half of
                                 //   the word and releases that paired with none in the low
                                 //   half. Both are 0 between calls
    KOS_ASPACE_OP_REENT_SEATING = 19, // () -> bit 0 when a thread whose task holds no space
                                 //   has been switched in, bit 1 when libc's reentrant state
                                 //   was written for such a thread. 1 is the only right answer
    KOS_ASPACE_OP_DATA_HOME_FORGET = 20, // () -> 0, having dropped the space that holds the
                                 //   image's own data pages, as its release would
    KOS_ASPACE_OP_MAP_TLBI = 21, // () -> page-invalidation sequences the map editor has
                                 //   ISSUED in the high half of the word and ELIDED in the
                                 //   low half, both since boot. A space installed on no core
                                 //   caches nothing, so seeding one lands wholly in the low
                                 //   half; a widening of the running space lands in the high
    KOS_ASPACE_OP_MAP_HERE = 22, // () -> a virtual address the CALLING task's own space now
                                 //   maps onto a fresh frame the kernel has seeded, or 0 when
                                 //   the scenario could not be set up. The caller reads that
                                 //   address ITSELF, through the running translation and at
                                 //   the unprivileged level
    KOS_ASPACE_OP_UNMAP_HERE = 23, // (the word the caller read back) -> 1 once the page
                                 //   KOS_ASPACE_OP_MAP_HERE handed out is unmapped from the
                                 //   calling task's space and its frame returned, 0 when the
                                 //   word is not the one the kernel seeded, which means the
                                 //   caller's read did not reach that frame. The caller's next
                                 //   read of the address must fault
    KOS_ASPACE_OP_ACQUIRE_DUP = 24, // () -> the KOS_ASPACE_DUP_* bits that held for two
                                 //   simultaneous acquires of ONE page, or 0 where the
                                 //   scenario could not be built
    KOS_ASPACE_OP_CAP_OBJECTS = 25, // () -> the KOS_ASPACE_CAPOBJ_* bits that held for the
                                 //   two object kinds the capability layer carries, a
                                 //   frame RUN and an address space: minted into the caller's
                                 //   own table, resolved back through the chokepoint, and
                                 //   closed. Every bit answers about a HANDLE and none of
                                 //   them is an address
    KOS_ASPACE_OP_CAP_SEED = 26, // () -> a frame capability in the low 32 bits and an
                                 //   address-space capability naming the CALLER's own space
                                 //   in the high 32, both minted into the caller's table, or
                                 //   0 when either could not be. Scaffolding: there is no
                                 //   user-facing mint yet, and the arm drives the REAL
                                 //   kos_frame_map and kos_frame_unmap syscalls on these
    KOS_ASPACE_OP_CAP_SEED_VA = 27, // () -> a page-aligned virtual address the seeded run may
                                 //   be mapped at, which nothing in the caller's space names
    KOS_ASPACE_OP_CAP_SELF_SPACE = 28, // () -> an address-space capability naming the CALLER's
                                 //   OWN space, minted into its table, or 0. A child task
                                 //   needs one for its own space and holds no other way to
                                 //   name it
    KOS_ASPACE_OP_CAP_RUN_REFS = 29, // () -> how many holders the last seeded frame RUN has,
                                 //   capabilities and MAPPINGS alike. A mapping is a holder:
                                 //   without that the last capability's drop frees frames a
                                 //   live leaf still points at
    KOS_ASPACE_OP_ACTIVE_CORES = 30, // () -> where the kernel's cores stand on translation
                                 //   roots, three fields in one word:
                                 //     23..16  cores one kernel schedules on, the denominator
                                 //     15..8   of those, the ones whose installed root is the
                                 //             boot root or a root some live domain holds
                                 //      7..0   cores the CALLING task's own space is installed
                                 //             on, which is its ACTIVE-CORE SET counted
                                 //   The middle field equalling the high one is the invariant
                                 //   that no core holds a root it is not running
    KOS_ASPACE_OP_AMP_ROUND = 32, // (node) -> drive ONE echo round at `node`: publish a
                                 //   request on the echo port and ring that node's doorbell.
                                 //   Returns the send's own verdict, 0 for accepted; the
                                 //   REPLY arrives later, through this node's own doorbell,
                                 //   so a caller reads KOS_ASPACE_OP_AMP_TOOK for it
    KOS_ASPACE_OP_AMP_FORGE = 33, // (selector) -> write ONE publication into THIS node's
                                 //   inbox exactly as a far side would and report what the
                                 //   validation made of it, or drive the send side's own
                                 //   refusal. The KOS_AMP_FORGE_* selectors say which
                                 //   malformation, and the KOS_AMP_V_* codes are the answers
                                 // The counter family below: ONE OP PER FIELD of `node`'s
                                 //   window record, each (node) -> that one counter, whole.
                                 //   A node outside the built range reads node 0's, so a
                                 //   caller may sweep a fixed width
    KOS_ASPACE_OP_AMP_TOOK = 34, // messages it took
    KOS_ASPACE_OP_AMP_DEPTH = 35, // takes refused on the far HEAD's depth
    KOS_ASPACE_OP_AMP_DEPTH_RESET = 36, // inboxes it resynchronised after DEPTH_STRIKES
                                 //   consecutive refused depths, which is what bounds how
                                 //   long a far side may keep one of its rings dead
    KOS_ASPACE_OP_AMP_LENGTH = 37, // slots refused on the far LENGTH
    KOS_ASPACE_OP_AMP_PORT = 38, // slots refused on the far PORT
    KOS_ASPACE_OP_AMP_SENT = 39, // messages it published
    KOS_ASPACE_OP_AMP_SEND_REFUSED = 40, // sends it refused
    KOS_ASPACE_OP_AMP_SERVICED = 41, // doorbell services that drained its inboxes
    KOS_ASPACE_OP_AMP_REPLY_DROP = 42, // replies taken and then refused by the tag validation
    KOS_ASPACE_OP_AMP_FAR_PARKED = 43, // () -> 1 while some thread is parked on a far reply,
                                 //   which is what the hostile-reply forges need to exist
                                 //   before they mean anything
    KOS_ASPACE_OP_AMP_FAR_EP = 44, // (port) -> a FAR endpoint capability naming node 1's
                                 //   `port`, minted into the caller's table, or 0. The same
                                 //   mint body with the privilege gate left out:
                                 //   KOS_SYS_AMP_ENDPOINT_CREATE is privileged and root is
                                 //   unprivileged from its first instruction, so on this board
                                 //   no user thread can reach that syscall
    KOS_ASPACE_OP_DOORBELL_COUNTS = 31 // (core) -> what `core` has done with the cross-core
                                 //   doorbell, two fields in one word:
                                 //     63..32  instruction-side rendezvous it INITIATED
                                 //     31..0   doorbell services it PERFORMED, each of which
                                 //             takes a Context synchronization event
                                 //   A core outside the built range reads 0, so a caller may
                                 //   sweep a fixed width. EVERY service counts here, a
                                 //   cross-core wake included, so the low field is an upper
                                 //   bound on the pokes answered
};

/* The ports a node mints in kernel init, which is what kos_amp_endpoint_create names. */
enum
{
    KOS_AMP_PORT_ECHO = 0, /* the payload comes back to the sender's reply port */
    KOS_AMP_PORT_REPLY = 1 /* a reply, routed to whatever local caller its tag names */
};

/* KOS_ASPACE_OP_AMP_FORGE: which malformation to write, and what the validation answers with.
   A selector this build does not know reads back KOS_AMP_V_EMPTY. */
enum
{
    KOS_AMP_FORGE_WELL_FORMED = 0, // the control: nothing malformed, so it must be taken
    KOS_AMP_FORGE_HEAD_DEPTH = 1,  // the far head names more outstanding slots than the ring
    KOS_AMP_FORGE_LENGTH = 2,      // the far length exceeds one slot
    KOS_AMP_FORGE_PORT = 3,        // the far port names nothing this node minted
    KOS_AMP_FORGE_PORT_WIDE = 4,   // the far port is outside the mint's own width
    KOS_AMP_FORGE_ZERO_LEN = 5,    // a zero-length message, which is one and not an empty ring
    KOS_AMP_FORGE_TAIL_DEPTH = 6,  // the SEND side's half: the far tail is the malformed index
    KOS_AMP_FORGE_SELF_SEND = 7,   // a send to the LOCAL node, whose ring nothing drains
    KOS_AMP_FORGE_DEPTH_RESET = 8, // a depth left standing, then a well-formed publication:
                                   // the answer is whether the ring recovered
    /* The four below play a PORT_REPLY at whatever thread is parked on a far call, which is
       the only way an arm reaches the tag validation's bounds. Each answers KOS_AMP_V_TOOK
       where the reply reached that caller and KOS_AMP_V_EMPTY where it was dropped. */
    KOS_AMP_FORGE_REPLY_UNPARKED = 9,   // a tag for a thread that is not parked at all
    KOS_AMP_FORGE_REPLY_WRONG_RING = 10, // the parked caller's own tag, on another node's ring
    KOS_AMP_FORGE_REPLY_STALE_SEQ = 11,  // the parked caller, one call sequence out of date
    KOS_AMP_FORGE_REPLY_GOOD = 12        // the control: the right tag on the right ring
};

enum
{
    KOS_AMP_V_EMPTY = 0,
    KOS_AMP_V_TOOK = 1,
    KOS_AMP_V_DEPTH = 2,
    KOS_AMP_V_LENGTH = 3,
    KOS_AMP_V_PORT = 4,
    KOS_AMP_V_SEND_OK = 16,
    KOS_AMP_V_SEND_DEPTH = 17,
    KOS_AMP_V_SEND_REFUSED = 18,
    KOS_AMP_V_SEND_NODE = 19
};

// KOS_ASPACE_OP_SPLIT_ACCESS: one bit per property of an access split at a page boundary.
/* KOS_ASPACE_OP_CAP_OBJECTS: one bit per property of the two capability kinds. The frames a
   run holds are the only thing here a number could leak, and none of these bits is one:
   every bit answers yes or no about a HANDLE. */
#define KOS_ASPACE_CAPOBJ_FRAME_MINT    0x01u /* a frame-run capability installed in this table */
#define KOS_ASPACE_CAPOBJ_FRAME_RESOLVE 0x02u /* it resolved back to the run that was minted */
#define KOS_ASPACE_CAPOBJ_ASPACE_MINT   0x04u /* an address-space capability installed */
#define KOS_ASPACE_CAPOBJ_ASPACE_HOLD   0x08u /* minting it took a hold on the domain */
#define KOS_ASPACE_CAPOBJ_ASPACE_STALE  0x10u /* a handle whose slot was reclaimed does NOT resolve */
#define KOS_ASPACE_CAPOBJ_CLOSE_FRAMES  0x20u /* closing the frame cap returned its frames */
#define KOS_ASPACE_CAPOBJ_CLOSE_HOLD    0x40u /* closing the space cap surrendered the hold */
#define KOS_ASPACE_CAPOBJ_BALANCED      0x80u /* the pool and the hold count are back where they began */
#define KOS_ASPACE_CAPOBJ_NO_REFUSED   0x100u /* the pool refused no free during the probe. A frame
                                                 handed back twice leaves the free COUNT balanced
                                                 and is visible only here */

#define KOS_ASPACE_SPLIT_NONADJACENT 0x01u /* the two virtually adjacent pages are not physically */
#define KOS_ASPACE_SPLIT_TO_USER     0x02u /* a straddling kaccess_to_user reached both frames */
#define KOS_ASPACE_SPLIT_FROM_USER   0x04u /* a straddling kaccess_from_user read both frames */
#define KOS_ASPACE_SPLIT_CROSS_SPACE 0x08u /* a straddling ep_copy between TWO spaces at ONE address */
#define KOS_ASPACE_SPLIT_NEIGHBOUR   0x10u /* the frame physically after the low page was untouched */
#define KOS_ASPACE_SPLIT_BALANCED    0x20u /* every frame the scenario took came back */
#define KOS_ASPACE_SPLIT_ALL         0x3Fu

// KOS_ASPACE_OP_ACQUIRE_DUP: one bit per property of two simultaneous holds of ONE page. A
// release names (space, page) and nothing else.
#define KOS_ASPACE_DUP_STABLE   0x01u /* two acquires of one page answered one pointer */
#define KOS_ASPACE_DUP_DISTINCT 0x02u /* another page acquired after one release got another */
#define KOS_ASPACE_DUP_REUSABLE 0x04u /* every hold surrendered, the page holdable again */
#define KOS_ASPACE_DUP_ALL      0x07u

// KOS_ASPACE_OP_MODEL: one bit per figure of the port's the machine bore out, with the figures
// it reported beside them. The widths are in BITS; the granule field carries one bit per granule
// the architecture defines, smallest first.
#define KOS_ASPACE_MODEL_GRANULE     0x01u /* the granule this port programs is supported */
#define KOS_ASPACE_MODEL_ASID        0x02u /* the identifier is as wide as the port's record */
#define KOS_ASPACE_MODEL_PA          0x04u /* the physical range covers what the port programs */
#define KOS_ASPACE_MODEL_ALL         0x07u
#define KOS_ASPACE_MODEL_ASID_SHIFT  8u
#define KOS_ASPACE_MODEL_PA_SHIFT    16u
#define KOS_ASPACE_MODEL_GRAN_SHIFT  24u
#define KOS_ASPACE_MODEL_FIELD_MASK  0xFFu

// KOS_ASPACE_OP_ROUNDTRIP: how far the cycle got, so a failure names its transition.
#define KOS_ASPACE_TRIP_MAPPED    1
#define KOS_ASPACE_TRIP_READBACK  2
#define KOS_ASPACE_TRIP_UNMAPPED  3
#define KOS_ASPACE_TRIP_GONE      4

// KOS_ASPACE_OP_REFUSALS: one bit per refusal the map editor must make.
#define KOS_ASPACE_REFUSE_HIGH_HALF     0x01u /* a kernel-half address */
#define KOS_ASPACE_REFUSE_UNALIGNED     0x02u /* a base below the granule */
#define KOS_ASPACE_REFUSE_EMPTY         0x04u /* zero pages */
#define KOS_ASPACE_REFUSE_NO_READ       0x08u /* write or execute with no read */
#define KOS_ASPACE_REFUSE_UNKNOWN_RIGHT 0x10u /* a bit outside ARCH_MAP_R/W/X */
#define KOS_ASPACE_REFUSE_PART_UNMAP    0x20u /* unmap of a range not wholly mapped */
#define KOS_ASPACE_REFUSE_WRITE_EXEC    0x40u /* writable and executable at once */
#define KOS_ASPACE_REFUSE_PHYS_EXTENT   0x80u /* a run reaching past the output-address width */
#define KOS_ASPACE_REFUSE_ALL           0xFFu

// KOS_ASPACE_OP_FORCED_UNWIND: one bit per property of the swept forced failure. The DEPTH
// is what stops the whole word passing vacuously: with every bit set and a depth of 0 the
// sweep injected nothing and refused nothing, so an arm reads the depth too.
#define KOS_ASPACE_UNWIND_REFUSED   0x01u /* every injected attempt refused the create */
#define KOS_ASPACE_UNWIND_ENOMEM    0x02u /* and every refusal answered exactly KOS_ENOMEM */
#define KOS_ASPACE_UNWIND_BALANCED  0x04u /* each refusal returned every frame, read at once */
#define KOS_ASPACE_UNWIND_NO_DOUBLE 0x08u /* the pool refused no free over the whole sweep */
#define KOS_ASPACE_UNWIND_SWEPT     0x10u /* the sweep ran past the last allocation a create makes */
#define KOS_ASPACE_UNWIND_REUSABLE  0x20u /* a create after the sweep succeeded and balanced */
#define KOS_ASPACE_UNWIND_ALL       0x3Fu
#define KOS_ASPACE_UNWIND_DEPTH_SHIFT 8
#define KOS_ASPACE_UNWIND_MIN_DEPTH 4

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

// `op` selector for KOS_SYS_GRANT_PROBE (self-test only). Values are a frozen
// contract: append, never reorder. Ops 0..4 return the predicate as 0/1; ops 5..7
// return a raw count / reserved-block base / size. A BAD op returns -KOS_EINVAL.
enum kos_grant_op
{
    KOS_GRANT_OP_HITS_RESERVED = 0,   // grant_hits_reserved(base, size)
    KOS_GRANT_OP_RAM_PRIVILEGED = 1,  // grant_region_admissible RAM, privileged caller
    KOS_GRANT_OP_RAM_UNPRIVILEGED = 2, // grant_region_admissible RAM, unprivileged caller
    KOS_GRANT_OP_DEV_PRIVILEGED = 3,  // grant_region_admissible DEV, privileged caller
    KOS_GRANT_OP_DEV_UNPRIVILEGED = 4, // grant_region_admissible DEV, unprivileged caller
    KOS_GRANT_OP_RESERVED_COUNT = 5,  // count of arch_reserved_blocks
    KOS_GRANT_OP_RESERVED_BASE = 6,   // reserved block[base].base (base indexes the block)
    KOS_GRANT_OP_RESERVED_SIZE = 7,   // reserved block[base].size (base indexes the block)
    KOS_GRANT_OP_NOCACHE_SUPPORT = 8, // arch_mpu_nocache_support() (enum arch_mpu_nocache)
    KOS_GRANT_OP_RAM_NOCACHE = 9      // grant_region_admissible RAM|NOCACHE, unprivileged
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
    // with -KOS_EPERM; a chip with no cache in that path accepts it; a chip that TRANSLATES
    // answers from its page tables. Honouring is checked: an accepted-but-unhonoured request is
    // silent data corruption, the caller having no cache-maintenance call to repair it with.
    KOS_MEM_NOCACHE = 1u << 0
};
#define KOS_MEM_FLAGS_ALL (KOS_MEM_NOCACHE)

// `op` selector for KOS_SYS_BENCH (KICKOS_BENCH images only). Values are a frozen
// contract: append, never reorder. A BAD op returns -KOS_EINVAL.
//
// The two PRINT ops make the KERNEL write the line, so they land on the kernel console
// (kickos_services_none) alone.
enum kos_bench_op
{
    KOS_BENCH_OP_RESET = 0,       // ()          -> 0. Switch AND phase accumulators.
    KOS_BENCH_OP_CORE_HZ = 1,     // ()          -> SystemCoreClock in Hz, 0 if unknown
    KOS_BENCH_OP_SWITCH_PRINT = 2, // ()         -> switch sample count (kernel prints the line)
    KOS_BENCH_OP_IRQ_SETUP = 3,   // (line)      -> 0
    KOS_BENCH_OP_IRQ_ONCE = 4,    // (line)      -> best-case inject->entry cycles, 0 = did
                                  //   not fire (no injectable line, or no cycle counter)
    KOS_BENCH_OP_IRQ_MASKED_ONCE = 5, // (line, span_bytes) -> worst-case inject->entry
                                  //   cycles across a masked span, 0 = did not fire
    KOS_BENCH_OP_PHASE_PRINT = 6  // ()          -> 0 (kernel prints the phase table)
};

// KOS_SYS_RECV's out-pointer: 8 bytes, 4-aligned. A plain kos_send arrival delivers
// reply_cap == KOS_CAP_NONE; a kos_call arrival delivers a one-shot reply cap handle the
// receiver must eventually kos_reply or kos_handle_close. A null out-ptr REJECTS calls, and
// the caller's kos_call fails -KOS_ENOSYS.
struct kos_recv_info
{
    uint32_t badge;      // sender badge (KOS_BADGE_NONE == 0 in this stage)
    kos_cap_t reply_cap; // KOS_CAP_NONE for a plain send; else a one-shot CAP_REPLY handle
                         // in the receiver's table. Test it against KOS_CAP_NONE: a live
                         // reply cap can have bit 31 set, so no sign test works.
};
#ifdef __cplusplus
static_assert(sizeof(struct kos_recv_info) == 8, "kos_recv_info must stay 8 bytes (ABI)");
#else
_Static_assert(sizeof(struct kos_recv_info) == 8, "kos_recv_info must stay 8 bytes (ABI)");
#endif

// KOS_SYS_RECV_TIMED's argument struct: the deadline plus, NESTED, the out-struct above.
// The kernel's write-back is a WHOLE-struct copy of the nested kos_recv_info, so it
// preserves no input word in it.
struct kos_recv_timed_opts
{
    uint32_t timeout_us;       // IN: relative microseconds, or KOS_TIMEOUT_NONE
    struct kos_recv_info info; // OUT: written exactly as a plain kos_recv writes it
};
#ifdef __cplusplus
static_assert(sizeof(struct kos_recv_timed_opts) == 12,
              "kos_recv_timed_opts must stay 12 bytes (ABI)");
static_assert(offsetof(struct kos_recv_timed_opts, info) == 4,
              "the nested kos_recv_info must sit at offset 4 (ABI)");
#else
_Static_assert(sizeof(struct kos_recv_timed_opts) == 12,
               "kos_recv_timed_opts must stay 12 bytes (ABI)");
_Static_assert(offsetof(struct kos_recv_timed_opts, info) == 4,
               "the nested kos_recv_info must sit at offset 4 (ABI)");
#endif

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
#ifdef __cplusplus
static_assert((unsigned)KOS_EP_MSG_MAX < KOS_CALL_LEN_MASK,
              "KOS_EP_MSG_MAX must stay strictly below the packed field's saturation value");
#else
_Static_assert((unsigned)KOS_EP_MSG_MAX < KOS_CALL_LEN_MASK,
               "KOS_EP_MSG_MAX must stay strictly below the packed field's saturation value");
#endif

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

// The robust-mutex "owner died" case is a NEGATIVE code: mutex_lock returns -KOS_EOWNERDEAD
// with the lock HELD. See the kos_mutex_lock decl for the held-vs-not-held caveat.

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

// Object capability rights (must mirror kickos::CapRights): the rights of a semaphore,
// mutex, endpoint or reply cap. A delegation NARROWS only: the child cap gets
// parent.rights & mask, and a mask adding a bit the parent lacks is rejected.
// Delegating requires the parent cap carry KOS_CAP_TRANSFER.
enum kos_cap_rights
{
    KOS_CAP_WAIT = 1 << 0,    // sem_wait; endpoint recv
    KOS_CAP_SIGNAL = 1 << 1,  // sem_post; endpoint send
    KOS_CAP_TRANSFER = 1 << 2 // may be delegated onward
};

// The thread's authority word (must mirror kickos::CapAuthority): its own field, sharing no
// numbering with kos_cap_rights. It is TCB state and not a table entry, so a spawning parent
// is what seats it. A thread may pass a bit to a child (kos_thread_params::authority) only if
// it holds that bit, and may drop bits with kos_cap_narrow(KOS_CAP_AUTHORITY, mask). Nothing
// widens.
enum kos_cap_authority
{
    KOS_AUTH_MEMORY = 1 << 0,  // kos_ram_alloc, the spawn-time MMIO grant, kos_mem_self_grant
    KOS_AUTH_PINMUX = 1 << 1,  // kos_pinmux_set
    KOS_AUTH_PSTATE = 1 << 2,  // kos_cpu_clock_set
    KOS_AUTH_IRQ = 1 << 3,     // kos_irq_claim, kos_irq_attach, kos_irq_unmask
    KOS_AUTH_SYSTEM = 1 << 4,  // kos_shutdown, kos_reboot
    KOS_AUTH_CONSOLE = 1 << 5  // kos_console_publish
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

// Thread-creation parameters (kernel allocates TCB + stack from a static pool).
struct kos_thread_params
{
    void (*entry)(void* arg);
    void* arg;
    char const* name;
    uint8_t prio;
    uint8_t policy;      // enum kos_policy
    uint8_t privileged;  // 0 => unprivileged user thread
    uint32_t quantum_ns; // RR slice; 0 => none
    void* mem_base;      // domain data region granted to the thread (0 => none). A block
                         // kos_ram_alloc handed the CALLER; the child's task maps it at the
                         // same address, and a range the caller never reserved is refused.
    uint32_t mem_size;   // size of that region (bytes)
    void* mmio_base;     // device/MMIO region granted to the thread (0 => none); attr implied R|W|DEV
                         // EXCLUSIVE for an unprivileged child: overlapping a window a live
                         // thread holds -> -KOS_EBUSY
    uint32_t mmio_size;  // size of that region (bytes)
    void* stack_base;    // caller-owned thread stack; 0 => kernel default (KICKOS_USER_STACK_SIZE).
                         // Must be memory the space the CHILD runs in already reaches: an
                         // in-arena block under an MPU, a kos_ram_alloc block made reachable
                         // in that task under translation. App static data is neither.
    uint32_t stack_size; // size of the caller stack (bytes); ignored when stack_base == 0
    struct kos_cap_grant const* caps; // optional caps to delegate to the child (0 => none)
    // OPTIONAL per-grant destination indices, cap_count entries parallel to caps[], or null
    // for "every cap takes its default index". An entry of 0 also means default: index 0 is
    // the kernel's stdout slot and cap_install_at refuses it outright.
    //
    // Default placement puts the first delegated cap at index 1, which is KOS_CAP_CLOCK's
    // well-known index, so a parent handing a child one cap aliases a reserved name unless
    // it names a destination here.
    //
    // -KOS_EINVAL, checked before the child exists, unless no two grants land on the same
    // index (the defaulted ones counted) and every index is below the width the child's
    // table ACTUALLY gets: KICKOS_CAP_CHILD_WIDTH where the table is chunked, and
    // KICKOS_MAX_HANDLES on a build whose whole table is one chunk.
    //
    // The array must be uint16_t-aligned; the kernel refuses it otherwise rather than take
    // a misaligned privileged load on a strict-align arch.
    uint16_t const* cap_dest;
    // Cores the child may run on, as a mask. 0 asks for its task's DEFAULT set, which is that
    // task's core set less the cores the image isolates; kos_thread_set_affinity resolves a
    // zero mask to the same set. Nothing reaches an isolated core without an explicit mask
    // naming it, here or through that call. Narrows and never widens: the
    // kernel intersects it with the task's set, and a mask meeting that set nowhere is
    // -KOS_EPERM. A bit naming a core the image does not drive is DROPPED, and only a mask
    // naming no core it drives at all is -KOS_EINVAL.
    //
    // IGNORED on an image whose kernel drives one core, where every mask names the same
    // placement.
    uint32_t core_mask;
    uint8_t cap_count;   // number of entries in caps[]; under default placement they land
                         // at child indices 1..cap_count.
                         // Above KICKOS_MAX_SPAWN_GRANTS: -KOS_EINVAL. That bound is the
                         // spawn stager's, NOT the child table's ceiling.
    // Authority bits (kos_cap_authority KOS_AUTH_*) to seat as the child's authority word;
    // 0 => none. Only a thread that already holds each bit may pass it: narrows, never
    // widens, like a cap_grant mask. This 8-bit field bounds the authority word to 8 bits.
    uint8_t authority;
    // The task the child JOINS, from kos_task_create, or KOS_TASK_NONE to make the child a
    // thread of the spawner's task (KOS_TASK_NONE above). Only the task's creator may seat a
    // member. A member shares the task's data region, so it may bring NO mem_base of its own
    // (-KOS_EINVAL) and may not be privileged (-KOS_EINVAL); mmio_base is per-thread and is
    // still its own. A named task is the only way to put a thread in a group of its own, and
    // a fault ends the whole task.
    kos_task_t task;
};

// sizeof is NOT assertable here: three pointers make it width-dependent (64 B on armv7m,
// 112 B on a 64-bit host). What is width-independent is the TAIL PACKING, and that is the
// part a new field would move silently: core_mask is the 32-bit word the placement syscalls
// carry, and cap_count and authority sit immediately behind it with no padding between.
#ifdef __cplusplus
static_assert(sizeof(((struct kos_thread_params*)0)->core_mask) == 4,
              "the core mask is a 32-bit word at every ABI that carries one");
static_assert(offsetof(struct kos_thread_params, cap_count)
                  == offsetof(struct kos_thread_params, core_mask) + 4,
              "cap_count must sit immediately behind core_mask (ABI)");
static_assert(offsetof(struct kos_thread_params, authority)
                  == offsetof(struct kos_thread_params, cap_count) + 1,
              "authority must sit immediately behind cap_count (ABI)");
#else
_Static_assert(sizeof(((struct kos_thread_params*)0)->core_mask) == 4,
               "the core mask is a 32-bit word at every ABI that carries one");
_Static_assert(offsetof(struct kos_thread_params, cap_count)
                   == offsetof(struct kos_thread_params, core_mask) + 4,
               "cap_count must sit immediately behind core_mask (ABI)");
_Static_assert(offsetof(struct kos_thread_params, authority)
                   == offsetof(struct kos_thread_params, cap_count) + 1,
               "authority must sit immediately behind cap_count (ABI)");
#endif

#endif
