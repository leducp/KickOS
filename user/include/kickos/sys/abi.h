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
// failure -- it cannot carry a negative errno in-band) and cpu_clock_hz / cpu_clock_set
// return a u32 Hz with a 0 == cannot/unknown sentinel; both stay OUT of the scheme.
enum kos_syscall_nr
{
    KOS_SYS_kconsole_write = 1, // (buf, len)            -> bytes written, or -KOS_EFAULT (bad buffer)
    KOS_SYS_yield = 2,          // ()                    -> 0
    KOS_SYS_sleep_ns = 3,       // (ns_lo, ns_hi)        -> 0
    KOS_SYS_sem_create = 4,     // (initial)             -> opaque sem handle, or -KOS_E* (ENOMEM)
    KOS_SYS_sem_wait = 5,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EPERM (C wrapper now surfaces it)
    KOS_SYS_sem_post = 6,       // (cap)   -> 0, or -KOS_EBADF/-KOS_EPERM (C wrapper now surfaces it)
    KOS_SYS_handle_close = 17,  // (cap)   -> 0, -KOS_EBADF (bad cap), -KOS_EBUSY (own a held mutex)
    KOS_SYS_thread_spawn = 7,   // (kos_thread_params*)  -> opaque thread handle, or -KOS_E*
    KOS_SYS_exit = 8,           // (code)                -> does not return
    KOS_SYS_irq_inject = 9,     // (irq)                 -> 0, or -KOS_EINVAL (self-test only)
    KOS_SYS_guard_addr = 10,    // ()  -> protected probe addr (self-test only)
    KOS_SYS_irq_attach = 11,    // (irq, sem_handle)  -> 0, or -KOS_E* (EPERM/EINVAL/EBADF/EBUSY)
    KOS_SYS_clock_now = 12,     // (uint64_t* out)       -> 0, or -KOS_EINVAL/-KOS_EFAULT (bad out-ptr)
    KOS_SYS_ram_alloc = 13,     // (size)                -> user-RAM ptr, or 0/NULL on ANY failure
    KOS_SYS_irq_register = 14,  // (line)                -> irq handle, or -KOS_E* (EINVAL/EBUSY/ENOMEM)
    KOS_SYS_irq_wait = 15,      // (handle)              -> 0, or -KOS_EBADF
    KOS_SYS_irq_ack = 16,       // (handle)              -> 0, or -KOS_EBADF
    KOS_SYS_irq_spurious = 18,  // ()  -> count of IRQs on unbound lines (self-test only)
    KOS_SYS_diag_led_set = 19,  // (on)                  -> 0 (kernel diagnostic LED)
    KOS_SYS_diag_led_toggle = 20, // ()                  -> 0 (kernel diagnostic LED)
    KOS_SYS_irq_unmask = 21,    // (irq)  -> 0, or -KOS_E* (EPERM/EINVAL; self-test only)
    KOS_SYS_cpu_clock_hz = 22,  // ()  -> running core clock in Hz (u32), 0 if unknown (NO KOS_E*)
    KOS_SYS_mutex_create = 23,  // ()     -> opaque mutex cap, or -KOS_ENOMEM (pool/table full)
    KOS_SYS_mutex_lock = 24,    // (cap)  -> 0 held; -KOS_EOWNERDEAD held-but-owner-died; -KOS_EBADF
                                //   / -KOS_EDEADLK NOT held (see the wrapper decl for the caveat)
    KOS_SYS_mutex_unlock = 25,  // (cap)  -> 0, -KOS_EBADF (bad cap), -KOS_EPERM (caller not owner)
    KOS_SYS_endpoint_create = 26, // ()                          -> endpoint cap, or -KOS_ENOMEM
    KOS_SYS_send = 27,          // (cap, buf, len)  -> bytes transferred, or -KOS_E* (see kos_send)
    KOS_SYS_recv = 28,          // (cap, buf, cap_len, u32* badge) -> bytes received, or -KOS_E*
    KOS_SYS_console_publish = 29, // (endpoint_cap) -> 0, -KOS_EPERM (not priv), -KOS_EBADF (bad cap)
    KOS_SYS_cpu_clock_set = 30,  // (kos_pstate_t as u32) -> landed core Hz (u32); 0 == cannot-change
    KOS_SYS_grant_probe = 31     // (op, base, size) -> Rule 7 grant predicate 0/1, or for ops 6/7
                                 //   the raw reserved-block base/size; a BAD op returns -KOS_EINVAL
                                 //   (self-test only; compiled out unless KICKOS_HAVE_MPU)
};

// P-state selector for KOS_SYS_cpu_clock_set. A fixed-width u32 enum (NOT a raw Hz):
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
enum kos_cap_rights
{
    KOS_CAP_WAIT = 1 << 0,    // sem_wait; endpoint recv
    KOS_CAP_SIGNAL = 1 << 1,  // sem_post; endpoint send
    KOS_CAP_TRANSFER = 1 << 2 // may be delegated onward
};

// One entry of a spawn delegation list: hand the child a narrowed copy of the
// parent cap `source_cap`. Deterministic placement (B1): delegated cap i lands at
// the child's table index i+1 (index 0 reserved), and a fresh child table has
// cap-gen 0 so the child's handle value == its index -- known a priori, no handoff.
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
};

#endif
