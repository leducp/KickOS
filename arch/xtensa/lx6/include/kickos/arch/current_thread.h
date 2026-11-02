// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The running thread in MISC0, one per core: the seating core writes it beside its current
// cell, and a read is one RSR, which no preemption can split.

#ifndef KICKOS_ARCH_CURRENT_THREAD_H
#define KICKOS_ARCH_CURRENT_THREAD_H

#if defined(__XTENSA__) && KICKOS_KERNEL_CORES > 1

#define KICKOS_ARCH_CURRENT_ONE_READ 1

static inline __attribute__((always_inline)) void* arch_current_load(void)
{
    void* t;
    __asm volatile("rsr.misc0 %0" : "=a"(t));
    return t;
}

static inline __attribute__((always_inline)) void arch_current_store(void* t)
{
    __asm volatile("wsr.misc0 %0; rsync" ::"a"(t) : "memory");
}

#endif

#endif
