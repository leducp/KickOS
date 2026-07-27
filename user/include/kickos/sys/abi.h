// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS syscall ABI shared by userspace stubs and the kernel dispatch
// table. Numbers are stable contract; argument packing is uintptr_t-wide.

#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/errno.h> // KOS_E* taxonomy: failures return -KOS_Exxx (see below)

// Return-encoding contract (see errno.h). A syscall that can fail returns its error
// as -KOS_Exxx (negative); success is a non-negative handle / byte-count / count, so
// the two are collision-free. EXCEPTIONS: ram_alloc returns a pointer (0/NULL on ANY
// failure, unable to carry a negative errno in-band) and cpu_clock_hz / cpu_clock_set /
// periph_clock_hz return a u32 Hz with a 0 == cannot/unknown sentinel; all stay OUT of the scheme.
enum kos_syscall_nr
{
    KOS_SYS_KCONSOLE_WRITE = 1, // (buf, len)            -> bytes written, or -KOS_EFAULT (bad buffer)
    KOS_SYS_YIELD = 2,          // ()                    -> 0
    KOS_SYS_SLEEP_NS = 3,       // (ns_lo, ns_hi)        -> 0
    KOS_SYS_SEM_CREATE = 4,     // (initial)             -> opaque sem handle, or -KOS_E* (ENOMEM)
    KOS_SYS_SEM_WAIT = 5,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EPERM (C wrapper now surfaces it)
    KOS_SYS_SEM_POST = 6,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EPERM (C wrapper now surfaces it)
    KOS_SYS_HANDLE_CLOSE = 17,  // (cap)   -> 0, -KOS_EBADF (bad cap), -KOS_EBUSY (own a held mutex)
    KOS_SYS_THREAD_SPAWN = 7,   // (kos_thread_params*)  -> opaque thread handle, or -KOS_E*
    KOS_SYS_EXIT = 8,           // (code)                -> does not return
    KOS_SYS_IRQ_INJECT = 9,     // (irq)                 -> 0, or -KOS_EINVAL (self-test only)
    KOS_SYS_GUARD_ADDR = 10,    // ()  -> protected probe addr (self-test only)
    KOS_SYS_IRQ_ATTACH = 11,    // (irq, sem_handle)  -> 0, or -KOS_E* (EPERM/EINVAL/EBADF/EBUSY)
    KOS_SYS_CLOCK_NOW = 12,     // (uint64_t* out)       -> 0, or -KOS_EINVAL/-KOS_EFAULT (bad out-ptr)
    KOS_SYS_RAM_ALLOC = 13,     // (size)                -> user-RAM ptr, or 0/NULL on ANY failure
    KOS_SYS_IRQ_REGISTER = 14,  // (line)                -> irq handle, or -KOS_E* (EINVAL/EBUSY/ENOMEM)
    KOS_SYS_IRQ_WAIT = 15,      // (handle)              -> 0, or -KOS_EBADF
    KOS_SYS_IRQ_ACK = 16,       // (handle)              -> 0, or -KOS_EBADF
    KOS_SYS_IRQ_SPURIOUS = 18,  // ()  -> count of IRQs on unbound lines (self-test only)
    KOS_SYS_DIAG_LED_SET = 19,  // (on)                  -> 0 (kernel diagnostic LED)
    KOS_SYS_DIAG_LED_TOGGLE = 20, // ()                  -> 0 (kernel diagnostic LED)
    KOS_SYS_IRQ_UNMASK = 21,    // (irq)  -> 0, or -KOS_E* (EPERM/EINVAL; self-test only)
    KOS_SYS_CPU_CLOCK_HZ = 22,  // ()  -> running core clock in Hz (u32), 0 if unknown (NO KOS_E*)
    KOS_SYS_MUTEX_CREATE = 23,  // ()     -> opaque mutex cap, or -KOS_ENOMEM (pool/table full)
    KOS_SYS_MUTEX_LOCK = 24,    // (cap)  -> 0 held; -KOS_EOWNERDEAD held-but-owner-died; -KOS_EBADF
                                //   / -KOS_EDEADLK NOT held (see the wrapper decl for the caveat)
    KOS_SYS_MUTEX_UNLOCK = 25,  // (cap)  -> 0, -KOS_EBADF (bad cap), -KOS_EPERM (caller not owner)
    KOS_SYS_ENDPOINT_CREATE = 26, // ()                          -> endpoint cap, or -KOS_ENOMEM
    KOS_SYS_SEND = 27,          // (cap, buf, len)  -> bytes transferred, or -KOS_E* (see kos_send)
    KOS_SYS_RECV = 28,          // (cap, buf, cap_len, kos_recv_info* out) -> bytes received, or -KOS_E*
    KOS_SYS_CONSOLE_PUBLISH = 29, // (endpoint_cap) -> 0, -KOS_EPERM (not priv), -KOS_EBADF (bad cap)
    KOS_SYS_CPU_CLOCK_SET = 30,  // (kos_pstate_t as u32) -> landed core Hz (u32); 0 == cannot-change
    KOS_SYS_GRANT_PROBE = 31,    // (op, base, size) -> Rule 7 grant predicate 0/1, or for ops 6/7
                                 //   the raw reserved-block base/size; a BAD op returns -KOS_EINVAL
                                 //   (self-test only; compiled out unless KICKOS_HAVE_MPU)
    KOS_SYS_PERIPH_CLOCK_HZ = 32, // (base) -> peripheral branch clock in Hz (u32), 0 if unknown (NO KOS_E*)
    KOS_SYS_PINMUX_SET = 33,  // (port, pin, func) -> 0, -KOS_EPERM (not priv), -KOS_EINVAL (range), -KOS_EBUSY (kernel-owned pin), -KOS_ENOSYS (no backend)
    KOS_SYS_CALL = 34,        // (ep_cap, buf, send_len, recv_cap) -> reply bytes (>= 0), or -KOS_E* (EINVAL/EFAULT/EBADF/EPERM/EPIPE/ENOMEM/ENOSYS)
    KOS_SYS_REPLY = 35,       // (reply_cap, buf, len) -> 0, or -KOS_E* (EBADF bad/non-reply cap, ESRCH stale caller, EFAULT bad buffer)
    KOS_SYS_SHUTDOWN = 36     // (status) -> does not return; -KOS_EPERM if refused
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
// plain kos_send arrival delivers reply_cap == -1; a kos_call arrival delivers a
// real one-shot reply cap handle (>= 0) the receiver must eventually kos_reply or
// kos_handle_close. A receiver that passes a null out-ptr (info-less recv) REJECTS
// calls (the caller's kos_call fails -KOS_ENOSYS) and behaves as before for plain sends.
struct kos_recv_info
{
    uint32_t badge;    // sender badge (KOS_BADGE_NONE == 0 in this stage)
    int32_t reply_cap; // -1 plain send; else a one-shot CAP_REPLY handle in the receiver's table
};
#ifdef __cplusplus
static_assert(sizeof(struct kos_recv_info) == 8, "kos_recv_info must stay 8 bytes (ABI)");
#else
_Static_assert(sizeof(struct kos_recv_info) == 8, "kos_recv_info must stay 8 bytes (ABI)");
#endif

// P-state selector for KOS_SYS_CPU_CLOCK_SET. A fixed-width u32 enum (NOT a raw Hz):
// the achievable set is small and chip-specific, and the truthful landed Hz is the
// syscall's return value. Carried as a plain u32 in the syscall register, so the width
// is the stable ABI -- append new states, never reorder. New deep-sleep states (STOP/
// STANDBY, tickless deep-sleep) append here later without an ABI break.
typedef enum kos_pstate_e : uint32_t
{
    KOS_PSTATE_MAX = 0, // full PLL (the boot clock: XMC 144 / K64F 120 MHz)
    KOS_PSTATE_MID,     // a reduced locked-PLL / staged point (chip rounds to nearest)
    KOS_PSTATE_LOW      // deep power saving (crystal/RC direct or a low staged point)
} kos_pstate_t;

// Shared payload bound: send REJECTS a len above this; recv clamps its capacity to it.
#define KOS_EP_MSG_MAX 256

// Counting-semaphore ceiling. The kernel keeps the count in an `int`, so this IS the
// type's range rather than a policy number: sem_create refuses an initial value outside
// [0, KOS_SEM_COUNT_MAX] with -KOS_EINVAL, and a post at the ceiling is refused with
// -KOS_EOVERFLOW. Signed overflow is undefined behaviour -- the compiler is entitled to
// assume it cannot happen -- and post is reachable from unprivileged code, so the count
// is bounded at the boundary instead of trusting the caller to stop.
#define KOS_SEM_COUNT_MAX 0x7FFFFFFF

// The robust-mutex "owner died" case is now a NEGATIVE code in the fleet taxonomy:
// mutex_lock returns -KOS_EOWNERDEAD (the lock IS held; the protected state may be
// torn). See <kickos/sys/errno.h> and the kos_mutex_lock decl for the held-vs-not-held
// caveat. (The old +1 KOS_MUTEX_OWNER_DIED sentinel is retired -- use -KOS_EOWNERDEAD.)

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

// Capability rights (must mirror kickos::CapRights). A delegation NARROWS only:
// the child cap gets parent.rights & mask; a mask adding a bit the parent lacks is
// rejected. Delegating requires the parent cap carry KOS_CAP_TRANSFER.
//
// The rights byte is SHARED between the two cap families, and which bits mean anything
// depends on the cap's TYPE. The low three below are the object rights (a semaphore,
// mutex, endpoint or reply cap). The five KOS_AUTH_* bits are meaningful ONLY on the
// authority cap at KOS_CAP_AUTHORITY, and they are the ENTIRE budget for the life of
// the type -- eight bits, three spent here, five left, five named. Anything wanting a
// sixth authority has to merge two of these, not add one.
enum kos_cap_rights
{
    KOS_CAP_WAIT = 1 << 0,    // sem_wait; endpoint recv
    KOS_CAP_SIGNAL = 1 << 1,  // sem_post; endpoint send
    KOS_CAP_TRANSFER = 1 << 2, // may be delegated onward

    KOS_AUTH_MEMORY = 1 << 3, // kos_ram_alloc + the spawn-time MMIO window grant
    KOS_AUTH_PINMUX = 1 << 4, // kos_pinmux_set
    KOS_AUTH_CLOCK = 1 << 5,  // kos_cpu_clock_set
    KOS_AUTH_IRQ = 1 << 6,    // kos_irq_attach, kos_irq_unmask
    KOS_AUTH_DEVICE = 1 << 7  // kos_console_publish, kos_shutdown, future periph enable
};

// One entry of a spawn delegation list: hand the child a narrowed copy of the
// parent cap `source_cap`. Deterministic placement (B1): delegated cap i lands at
// the child's table index i+1 (index 0 reserved), and a fresh child table has
// cap-gen 0 so the child's handle value == its index -- known a priori, no handoff.
//
// Table index of the FIRST delegated cap (i == 0). A driver spawned with one
// delegated recv cap reads it here with no handoff; delegated cap i is at
// KOS_SPAWN_DELEGATED_CAP0 + i.
#define KOS_SPAWN_DELEGATED_CAP0 1
struct kos_cap_grant
{
    int source_cap;      // a cap handle in the SPAWNING thread's table
    uint8_t rights_mask; // subset of the source cap's rights (kos_cap_rights bits)
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
    uint32_t mmio_size;  // size of that region (bytes)
    void* stack_base;    // caller-owned thread stack; 0 => kernel default (KICKOS_USER_STACK_SIZE)
    uint32_t stack_size; // size of the caller stack (bytes); ignored when stack_base == 0
    struct kos_cap_grant const* caps; // optional caps to delegate to the child (0 => none)
    uint8_t cap_count;   // number of entries in caps[]; caps land at child indices 1..cap_count
    // Authority bits (kos_cap_rights KOS_AUTH_*) to seat as the child's authority cap at
    // KOS_CAP_AUTHORITY; 0 => none. Only a thread that already holds each bit may pass it,
    // so this narrows and never widens, exactly like a cap_grant mask. Fits in the padding
    // after cap_count, so the struct does not grow.
    //
    // Rejected with -KOS_EINVAL together with cap_count >= KOS_CAP_AUTHORITY: delegated cap
    // i lands at child index i+1, so a second delegated cap would land on the authority
    // slot. Refusing the pair is the honest reading until per-grant destination indices
    // land (see <kickos/sys/cap_index.h>); silently letting one overwrite the other is not.
    uint8_t authority;
};

#endif
