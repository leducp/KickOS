// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS syscall ABI shared by userspace stubs and the kernel dispatch
// table. Numbers are stable contract; argument packing is uintptr_t-wide.

#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/cap_index.h> // KOS_CAP_AUTHORITY, the well-known indices
#include <kickos/sys/errno.h> // KOS_E* taxonomy: failures return -KOS_Exxx (see below)

// Return-encoding contract (see errno.h). A syscall that can fail returns its error
// as -KOS_Exxx (negative); success is a non-negative byte-count / count, so the two are
// collision-free. EXCEPTIONS: ram_alloc returns a pointer (0/NULL on ANY failure, unable
// to carry a negative errno in-band) and cpu_clock_hz / cpu_clock_set / periph_clock_hz
// return a u32 Hz with a 0 == cannot/unknown sentinel; all stay OUT of the scheme.

// A capability handle. 16 index bits + 16 generation bits, so a live handle spends the
// WHOLE 32-bit word and may have bit 31 set: `h < 0` is not an error test on a capability,
// and every capability-MINTING call returns a status and delivers the handle through an
// out-parameter.
typedef uint32_t kos_cap_t;

// "No capability". The codec reserves the all-ones index and never seats a slot on it,
// so no table can mint this word (nor KOS_CAP_AUTHORITY, which shares that index field).
// Written to a minting call's out-parameter on EVERY failure, and carried by
// kos_recv_info.reply_cap for a plain send.
#define KOS_CAP_NONE 0xFFFFFFFFu

// A thread handle: 16 index bits + 16 generation bits over the THREAD pool, whose slots and
// generations are unrelated to kos_cap_t's. Both are plain 32-bit words, so the compiler
// will NOT catch a cap handle passed where a thread handle belongs; it just resolves against
// the wrong table. A slot aged past 32768 reclaims mints a handle with bit 31 set, so
// `h < 0` is not an error test here either.
typedef uint32_t kos_thread_t;

// "No thread". The thread pool never seats the all-ones index (kernel thread.h ties the two
// with a static_assert), so no generation can mint this word.
#define KOS_THREAD_NONE 0xFFFFFFFFu

enum kos_syscall_nr
{
    KOS_SYS_KCONSOLE_WRITE = 1, // (buf, len)            -> bytes written, or -KOS_EFAULT (bad buffer)
    KOS_SYS_YIELD = 2,          // ()                    -> 0
    KOS_SYS_SLEEP_NS = 3,       // (ns_lo, ns_hi)        -> 0
    KOS_SYS_SEM_CREATE = 4,     // (initial, kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM sem pool,
                                //   EMFILE caller's cap table, EINVAL/EFAULT)
    KOS_SYS_SEM_WAIT = 5,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EPERM (C wrapper now surfaces it)
    KOS_SYS_SEM_POST = 6,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EPERM (C wrapper now surfaces it)
    KOS_SYS_HANDLE_CLOSE = 17,  // (cap)   -> 0, -KOS_EBADF (bad cap), -KOS_EBUSY (own a held mutex)
    KOS_SYS_THREAD_SPAWN = 7,   // (kos_thread_params*, kos_thread_t* out) -> 0, or -KOS_E*
                                //   (EINVAL/EFAULT/EPERM/EBADF/EBUSY/ENOMEM/EOVERFLOW)
    KOS_SYS_EXIT = 8,           // (code)                -> does not return
    KOS_SYS_IRQ_INJECT = 9,     // (irq)                 -> 0, or -KOS_EINVAL (self-test only)
    KOS_SYS_GUARD_ADDR = 10,    // ()  -> protected probe addr (self-test only)
    KOS_SYS_IRQ_ATTACH = 11,    // (irq, sem_handle)  -> 0, or -KOS_E* (EPERM/EINVAL/EBADF/EBUSY)
    KOS_SYS_CLOCK_NOW = 12,     // (uint64_t* out)       -> 0, or -KOS_EINVAL/-KOS_EFAULT (bad out-ptr)
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
    KOS_SYS_SEND = 27,          // (cap, buf, len)  -> bytes transferred, or -KOS_E* (see kos_send)
    KOS_SYS_RECV = 28,          // (cap, buf, cap_len, kos_recv_info* out) -> bytes received, or -KOS_E*
    KOS_SYS_CONSOLE_PUBLISH = 29, // (endpoint_cap) -> 0, -KOS_EPERM (not priv), -KOS_EBADF (bad
                                  //   cap), -KOS_EOVERFLOW (endpoint refcount at its ceiling)
    KOS_SYS_CPU_CLOCK_SET = 30,  // (kos_pstate_t as u32) -> landed core Hz (u32); 0 == cannot-change
    KOS_SYS_GRANT_PROBE = 31,    // (op, base, size) -> Rule 7 grant predicate 0/1, or for ops 6/7
                                 //   the raw reserved-block base/size; a BAD op returns -KOS_EINVAL
                                 //   (self-test only; compiled out unless KICKOS_HAVE_MPU)
    KOS_SYS_PERIPH_CLOCK_HZ = 32, // (base) -> peripheral branch clock in Hz (u32), 0 if unknown (NO KOS_E*)
    KOS_SYS_PINMUX_SET = 33,  // (port, pin, func) -> 0, -KOS_EPERM (not priv), -KOS_EINVAL (range), -KOS_EBUSY (kernel-owned pin), -KOS_ENOSYS (no backend)
    KOS_SYS_CALL = 34,        // (ep_cap, buf, send_len, recv_cap) -> reply bytes (>= 0), or -KOS_E* (EINVAL/EFAULT/EBADF/EPERM/EPIPE/ENOSYS,
                              //   EMFILE the SERVER's cap table has no slot for the reply cap)
    KOS_SYS_REPLY = 35,       // (reply_cap, buf, len) -> 0, or -KOS_E* (EBADF bad/non-reply cap, ESRCH stale caller, EFAULT bad buffer)
    KOS_SYS_SHUTDOWN = 36,    // (status) -> does not return; -KOS_EPERM if refused
    KOS_SYS_MEM_SELF_GRANT = 37, // (base, size) -> 0, or -KOS_E* (EPERM/EINVAL/ENOMEM)
    KOS_SYS_REBOOT = 38,      // () -> does not return; -KOS_EPERM if refused, -KOS_ENOSYS (no backend)
                              //   (self-test only: the dispatch arm is compiled out unless
                              //   KICKOS_ENABLE_SELFTEST, so a production image returns -KOS_EINVAL)
    KOS_SYS_PERIPH_ENABLE = 39, // (base) -> 0, -KOS_EPERM (caller holds no window at that base),
                                //   -KOS_EINVAL (no table entry), -KOS_ENOSYS (no backend).
                                //   Gated on possession, not on an authority bit.
    KOS_SYS_CAP_NARROW = 40,   // (cap, mask) -> 0, -KOS_EBADF (bad cap), -KOS_EINVAL (not the
                               //   authority cap). UNGATED: dropping authority needs none.
    KOS_SYS_PANIC = 41,        // (msg) -> does not return. UNGATED: a thread that must
                               //   abort has to be able to say why. msg is copied into
                               //   kernel memory bounded + byte-checked; a message the
                               //   kernel cannot read is replaced, never dereferenced.
    KOS_SYS_PERIPH_REG_WRITE = 42, // (base, offset, value) -> 0, -KOS_EPERM (caller holds no
                               //   window at that base), -KOS_EINVAL (base+offset is not on
                               //   this chip's allowlist), -KOS_ENOSYS (no backend). Gated on
                               //   possession of the block at `base`, not on an authority bit.
    KOS_SYS_IRQ_NOTIFY = 43,   // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks SIGNAL).
                               //   Software-posts the binding WITHOUT touching the controller:
                               //   the TX doorbell, not a simulated device raise.
    KOS_SYS_IRQ_DISCARD = 44,  // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks WAIT).
                               //   Drops whatever the controller has latched for the line.
                               //   Neither masks nor unmasks.
    KOS_SYS_THREAD_KILL = 45   // (kos_thread_t) -> 0, -KOS_EBADF (bad/stale/exited handle),
                               //   -KOS_EPERM (the caller did not spawn that thread),
                               //   -KOS_EINVAL (naming yourself; that is KOS_SYS_EXIT).
                               //   COOPERATIVE: it marks the target and wakes it out of an
                               //   irq_wait with -KOS_ECANCELED; the target exits itself.
};

// Flags for KOS_SYS_IRQ_CLAIM. The trigger type is a property of the SOURCE, so it is
// fixed at claim time and never changes for the binding's life.
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
    KOS_GRANT_OP_RESERVED_SIZE = 7    // reserved block[base].size (base indexes the block)
};

// Widened KOS_SYS_RECV out-pointer (was a bare u32 badge). 8 bytes, 4-aligned. A
// plain kos_send arrival delivers reply_cap == KOS_CAP_NONE; a kos_call arrival delivers
// a real one-shot reply cap handle the receiver must eventually kos_reply or
// kos_handle_close. A receiver that passes a null out-ptr (info-less recv) REJECTS
// calls (the caller's kos_call fails -KOS_ENOSYS) and behaves as before for plain sends.
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

// P-state selector for KOS_SYS_CPU_CLOCK_SET. A fixed-width u32 enum (NOT a raw Hz):
// the achievable set is small and chip-specific, and the truthful landed Hz is the
// syscall's return value. Carried as a plain u32 in the syscall register, so the width
// is the stable ABI: append new states, never reorder. New deep-sleep states (STOP/
// STANDBY, tickless deep-sleep) append here later without an ABI break.
typedef enum kos_pstate_e : uint32_t
{
    KOS_PSTATE_MAX = 0, // full PLL (the boot clock: XMC 144 / K64F 120 MHz)
    KOS_PSTATE_MID,     // a reduced locked-PLL / staged point (chip rounds to nearest)
    KOS_PSTATE_LOW      // deep power saving (crystal/RC direct or a low staged point)
} kos_pstate_t;

// Shared payload bound: send REJECTS a len above this; recv clamps its capacity to it.
#define KOS_EP_MSG_MAX 256

// Counting-semaphore ceiling. The kernel keeps the count in an `int`, so this is the
// type's range rather than a policy number. sem_create refuses an initial outside
// [0, KOS_SEM_COUNT_MAX] with -KOS_EINVAL; a post at the ceiling is refused with
// -KOS_EOVERFLOW.
#define KOS_SEM_COUNT_MAX 0x7FFFFFFF

// The robust-mutex "owner died" case is a NEGATIVE code: mutex_lock returns
// -KOS_EOWNERDEAD (the lock IS held; the protected state may be torn). See
// <kickos/sys/errno.h> and the kos_mutex_lock decl for the held-vs-not-held caveat.

// 64-bit values are passed/returned as two uintptr_t halves so the ABI is
// identical on 32-bit (ARM M-class) and 64-bit (sim) targets: never rely on
// uintptr_t being 64 bits. sleep_ns takes (lo, hi); clock_now writes its u64
// result through a caller-supplied out-pointer. Helpers:
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

// The thread's authority word (must mirror kickos::CapAuthority): its own field,
// sharing no numbering with kos_cap_rights. It is TCB state, not a capability: there is
// no table entry for it and nothing can delegate it. A thread may pass a bit to a
// child (kos_thread_params::authority) only if it holds that bit, and may drop bits with
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

// KOS_AUTH_PSTATE and KOS_AUTH_CONSOLE are each separate bits: a CPU-governor service
// and a console driver thread are distinct from the thread that ends the system, and
// each holds only the authority it needs.

// kos_periph_enable carries no authority bit. It is gated on possession of the window
// it names, because its callers are the drivers: an authority bit would hand every
// unprivileged driver whatever else that bit covers.

// One entry of a spawn delegation list: hand the child a narrowed copy of the
// parent cap `source_cap`. Deterministic placement (B1): a fresh child table has cap-gen
// 0, so the child's handle value EQUALS its index and is known a priori with no handoff.
// Every driver bring-up and every service header is built on that.
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
                         // EXCLUSIVE: overlapping a window a live thread holds -> -KOS_EBUSY
    uint32_t mmio_size;  // size of that region (bytes)
    void* stack_base;    // caller-owned thread stack; 0 => kernel default (KICKOS_USER_STACK_SIZE)
    uint32_t stack_size; // size of the caller stack (bytes); ignored when stack_base == 0
    struct kos_cap_grant const* caps; // optional caps to delegate to the child (0 => none)
    uint8_t cap_count;   // number of entries in caps[]; under default placement they land
                         // at child indices 1..cap_count.
                         // Above KICKOS_MAX_SPAWN_GRANTS: -KOS_EINVAL. That bound is the
                         // spawn stager's, NOT the child table's ceiling.
    // OPTIONAL per-grant destination indices, cap_count entries parallel to caps[], or
    // null for "every cap takes its default index". An entry of 0 also means default, and
    // 0 can never be a real destination: index 0 is the kernel's stdout slot and
    // cap_install_at refuses it outright.
    //
    // Default placement puts the first delegated cap at index 1, which is KOS_CAP_CLOCK's
    // well-known index, so a parent handing a child one cap aliases a reserved name unless
    // it names a destination here.
    //
    // Checked before the child exists, so a bad list costs -KOS_EINVAL and not a half-built
    // thread: no two grants may land on the same index, counting the defaulted ones, and
    // every index is below the child's table size (KICKOS_MAX_HANDLES on this board).
    //
    // The array must be uint16_t-aligned; the kernel refuses it otherwise rather than take
    // a misaligned privileged load on a strict-align arch.
    uint16_t const* cap_dest;
    // Authority bits (kos_cap_authority KOS_AUTH_*) to seat as the child's authority
    // word; 0 => none. Only a thread that already holds each bit may pass it: narrows,
    // never widens, like a cap_grant mask. This 8-bit field is what bounds the authority
    // word to 8 bits.
    uint8_t authority;
};

#endif
