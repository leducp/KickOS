// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The running thread in the gs block, one per core: the seating core writes it beside its
// current cell, and a read is one gs-relative load, which no preemption can split. Valid once
// ring3_init or ring3_cpu_init has installed this core's gs base, as arch_cpu_id is.

#ifndef KICKOS_ARCH_CURRENT_THREAD_H
#define KICKOS_ARCH_CURRENT_THREAD_H

#if defined(__x86_64__) && __STDC_HOSTED__ == 0 && KICKOS_KERNEL_CORES > 1

#include <kickos/arch/ring3.h>

#define KICKOS_ARCH_CURRENT_ONE_READ 1

static inline __attribute__((always_inline)) void* arch_current_load(void)
{
    void* t;
    __asm volatile("movq %%gs:%c1, %0" : "=r"(t) : "i"(KICKOS_X86_64_CPU_CURRENT));
    return t;
}

static inline __attribute__((always_inline)) void arch_current_store(void* t)
{
    __asm volatile("movq %0, %%gs:%c1" ::"r"(t), "i"(KICKOS_X86_64_CPU_CURRENT) : "memory");
}

#endif

#endif
