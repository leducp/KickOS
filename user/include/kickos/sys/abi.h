// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS syscall ABI shared by userspace stubs and the kernel dispatch
// table. Numbers are stable contract; argument packing is uintptr_t-wide.

#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H

#include <stddef.h>
#include <stdint.h>

enum kos_syscall_nr
{
    KOS_SYS_kconsole_write = 1, // (buf, len)            -> bytes written (debug console)
    KOS_SYS_yield = 2,          // ()                    -> 0
    KOS_SYS_sleep_ns = 3,       // (ns_lo, ns_hi)        -> 0
    KOS_SYS_sem_create = 4,     // (initial)             -> opaque sem handle, or -1
    KOS_SYS_sem_wait = 5,       // (cap)      -> 0, or -1 bad cap (void C wrapper drops it)
    KOS_SYS_sem_post = 6,       // (cap)      -> 0, or -1 bad cap (void C wrapper drops it)
    KOS_SYS_handle_close = 17,  // (cap)   -> 0, or -1 (bad cap); type-agnostic, refcounted close
    KOS_SYS_thread_spawn = 7,   // (kos_thread_params*)  -> opaque thread handle, or -1
    KOS_SYS_exit = 8,           // (code)                -> does not return
    KOS_SYS_irq_inject = 9,     // (irq)                 -> 0
    KOS_SYS_guard_addr = 10,    // ()  -> protected probe addr (self-test only)
    KOS_SYS_irq_attach = 11,    // (irq, sem_handle)     -> 0, or -1 on bad irq/handle
    KOS_SYS_clock_now = 12,     // (uint64_t* out)       -> 0
    KOS_SYS_ram_alloc = 13,     // (size)                -> user-RAM ptr, or 0
    KOS_SYS_irq_register = 14,  // (line)                -> irq handle, or -1
    KOS_SYS_irq_wait = 15,      // (handle)              -> 0, or -1 on bad handle
    KOS_SYS_irq_ack = 16,       // (handle)              -> 0, or -1 on bad handle
    KOS_SYS_irq_spurious = 18,  // ()  -> count of IRQs on unbound lines (self-test only)
    KOS_SYS_diag_led_set = 19,  // (on)                  -> 0 (kernel diagnostic LED)
    KOS_SYS_diag_led_toggle = 20, // ()                  -> 0 (kernel diagnostic LED)
    KOS_SYS_irq_unmask = 21,    // (irq)  -> 0, or -1 (enable a line; self-test only)
    KOS_SYS_cpu_clock_hz = 22,  // ()  -> running core clock in Hz (u32), 0 if unknown
    KOS_SYS_mutex_create = 23,  // ()     -> opaque mutex cap, or -1 (pool/table full)
    KOS_SYS_mutex_lock = 24,    // (cap)  -> 0, 1 (owner died holding it), -1 bad cap, -2 deadlock
    KOS_SYS_mutex_unlock = 25   // (cap)  -> 0, or -1 (bad cap, or caller not owner)
};

// mutex_lock return: the previous owner exited while holding the mutex; this caller
// now owns it, but the protected invariant may be inconsistent (POSIX EOWNERDEAD).
// Must match kickos::MUTEX_OWNER_DIED.
#define KOS_MUTEX_OWNER_DIED 1

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
    KOS_CAP_WAIT = 1 << 0,    // sem_wait
    KOS_CAP_SIGNAL = 1 << 1,  // sem_post
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
