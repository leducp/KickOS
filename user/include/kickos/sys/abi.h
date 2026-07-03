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
    KOS_SYS_sem_wait = 5,       // (handle)   -> 0, or -1 bad handle (void C wrapper drops it)
    KOS_SYS_sem_post = 6,       // (handle)   -> 0, or -1 bad handle (void C wrapper drops it)
    KOS_SYS_sem_destroy = 17,   // (handle)              -> 0, or -1 (bad/has waiters); nr appended
    KOS_SYS_thread_spawn = 7,   // (kos_thread_params*)  -> thread id, or -1
    KOS_SYS_exit = 8,           // (code)                -> does not return
    KOS_SYS_irq_inject = 9,     // (irq)                 -> 0
    KOS_SYS_guard_addr = 10,    // ()  -> protected probe addr (self-test only)
    KOS_SYS_irq_attach = 11,    // (irq, sem_handle)     -> 0, or -1 on bad irq/handle
    KOS_SYS_clock_now = 12,     // (uint64_t* out)       -> 0
    KOS_SYS_ram_alloc = 13,     // (size)                -> user-RAM ptr, or 0
    KOS_SYS_irq_register = 14,  // (line)                -> irq handle, or -1
    KOS_SYS_irq_wait = 15,      // (handle)              -> 0, or -1 on bad handle
    KOS_SYS_irq_ack = 16,       // (handle)              -> 0, or -1 on bad handle
    KOS_SYS_irq_spurious = 18   // ()  -> count of IRQs on unbound lines (self-test only)
};

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
};

#endif
