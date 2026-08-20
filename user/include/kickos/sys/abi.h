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
// collision-free. EXCEPTIONS, all OUT of the scheme: ram_alloc returns a pointer, 0/NULL on
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

// "No task", and the spawn default: a thread naming no task gets an implicit one holding
// itself.
#define KOS_TASK_NONE 0u

// The exit code a thread killed by a CPU fault reports: what a joiner reads back, and the
// process status when it was the last thread live. Distinct from kfault_terminate's 132, so
// a capture tells a survived fault from a panic. A clean kos_exit(139) aliases it.
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
    KOS_SYS_THREAD_SPAWN = 7,   // (kos_thread_params*, kos_thread_t* out) -> 0, or -KOS_E*
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
                               //   -KOS_EPERM to any thread but root. Takes NO deadline.
    KOS_SYS_SEND_TIMED = 50,   // (cap, buf, len, timeout_us) -> as KOS_SYS_SEND, plus
                               //   -KOS_ETIMEDOUT
    KOS_SYS_TASK_CREATE = 51,  // (mem_base, mem_size, kos_task_t* out, kos_mem_flags) -> 0,
                               //   or -KOS_E*: EPERM (inadmissible shared grant, a memory
                               //   type this chip cannot honour, or a caller no member could
                               //   name), EINVAL (the window wraps, or an undefined flag
                               //   bit), ENOMEM (task or domain pool full), EFAULT (bad
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
                               //   the reply delivered in registers too. INTERNAL: no stub
                               //   spells it, kos_call selects it on size alone. Implemented
                               //   ONLY in the trap-handler fastpath; the generic dispatch
                               //   answers KOS_CALL_REG_FALLBACK, the stub's cue to re-issue
                               //   as KOS_SYS_CALL.
    KOS_SYS_IPC_FAST_TAKEN = 57, // ()  -> count of calls the trap-handler IPC fastpath
                               //   COMPLETED (self-test only). The fastpath and the buffer
                               //   form answer a caller identically, so this counter is the
                               //   only thing that separates them. Reads 0 on a backend with
                               //   no fastpath.
    KOS_SYS_NEST_WITNESS = 58  // (which) -> one nested-trap counter (self-test only), or
                               //   KOS_NEST_UNSET for a figure nothing recorded. A COUNTER
                               //   READ and not a report: a kernel-side report puts the
                               //   console's varargs route inside the SYSCALL red zone.
};

// Selectors for KOS_SYS_NEST_WITNESS. NEST_ROOM is the bytes between the lowest nested frame
// seen on a thread stack and that stack's base; compare it against the arch's own interrupt
// red zone, since less than that means the ISR below the frame had no bound.
#define KOS_NEST_TRAPS   0
#define KOS_NEST_ONSTACK 1
#define KOS_NEST_ROOM    2
#define KOS_NEST_UNSET   0xFFFFFFFFu

// The generic dispatch's answer for KOS_SYS_CALL_REG: retry through KOS_SYS_CALL. Outside
// the result range by construction, a kos_call answering a byte count in [0, KOS_EP_MSG_MAX]
// or a small negative errno, so it can never collide with a real answer.
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
//
// The flag belongs to the BLOCK: pass it identically to EVERY call that maps it, or two live
// mappings end up disagreeing about its type.
enum kos_mem_flags
{
    // Map the block Normal non-cacheable, for a block a bus master reads or writes. A chip
    // whose region descriptors carry no memory type and whose data cache sits over the arena
    // REFUSES it with -KOS_EPERM; a chip with no cache in that path accepts it. There is no
    // cache-maintenance call anywhere in this tree, so an unhonoured request would be silent
    // data corruption.
    KOS_MEM_NOCACHE = 1u << 0
};
#define KOS_MEM_FLAGS_ALL (KOS_MEM_NOCACHE)

// `op` selector for KOS_SYS_BENCH (KICKOS_BENCH images only). Values are a frozen
// contract: append, never reorder. A BAD op returns -KOS_EINVAL.
//
// The two PRINT ops make the KERNEL write the line, so they need the kernel console
// (kickos_services_none): under a published userspace console driver they reach nothing.
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
// An enum's own width is implementation-defined before C23 (arm-none-eabi short-enums this
// one to a single byte; GNURX does not), and a fixed underlying type is not ISO C11, so the
// ABI type is the u32 and not the enum. A caller wanting -Wswitch exhaustiveness back
// switches on `(enum kos_pstate_e)x`; the u32 alone carries no enumerator set.
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

// SATURATES at the field width instead of masking: a masked 512 would arrive as 0 and become
// a silent zero-length call, while a saturated 511 still trips the kernel's
// send_len > KOS_EP_MSG_MAX refusal.
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
// numbering with kos_cap_rights. It is TCB state, not a capability: there is no table entry
// for it and nothing can delegate it. A thread may pass a bit to a child
// (kos_thread_params::authority) only if it holds that bit, and may drop bits with
// kos_cap_narrow(KOS_CAP_AUTHORITY, mask). Nothing widens.
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
    void* mem_base;      // domain data region granted to the thread (0 => none)
    uint32_t mem_size;   // size of that region (bytes)
    void* mmio_base;     // device/MMIO region granted to the thread (0 => none); attr implied R|W|DEV
                         // EXCLUSIVE for an unprivileged child: overlapping a window a live
                         // thread holds -> -KOS_EBUSY
    uint32_t mmio_size;  // size of that region (bytes)
    void* stack_base;    // caller-owned thread stack; 0 => kernel default (KICKOS_USER_STACK_SIZE)
    uint32_t stack_size; // size of the caller stack (bytes); ignored when stack_base == 0
    struct kos_cap_grant const* caps; // optional caps to delegate to the child (0 => none)
    uint8_t cap_count;   // number of entries in caps[]; under default placement they land
                         // at child indices 1..cap_count.
                         // Above KICKOS_MAX_SPAWN_GRANTS: -KOS_EINVAL. That bound is the
                         // spawn stager's, NOT the child table's ceiling.
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
    // Authority bits (kos_cap_authority KOS_AUTH_*) to seat as the child's authority word;
    // 0 => none. Only a thread that already holds each bit may pass it: narrows, never
    // widens, like a cap_grant mask. This 8-bit field bounds the authority word to 8 bits.
    uint8_t authority;
    // The task the child JOINS, from kos_task_create, or KOS_TASK_NONE for an implicit task
    // holding the child alone. Only the task's creator may seat a member. A member shares the
    // task's data region and dies with the group, so it may bring NO mem_base of its own
    // (-KOS_EINVAL) and may not be privileged (-KOS_EINVAL); mmio_base is per-thread and is
    // still its own.
    kos_task_t task;
};

#endif
