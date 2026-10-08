// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ARM_COMMON_SEMIHOST_TRAP_H
#define KICKOS_ARCH_ARM_COMMON_SEMIHOST_TRAP_H

namespace kickos::semihost
{
    inline __attribute__((always_inline)) long call(long op, void* arg)
    {
        register long r0 __asm("r0") = op;
        register void* r1 __asm("r1") = arg;
        __asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
        return r0;
    }

    inline __attribute__((always_inline, noreturn)) void halt_masked(void)
    {
        __asm volatile("cpsid i" ::: "memory");
        while (true)
        {
            __asm volatile("wfi");
        }
    }
}

#endif
