// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ATOMIC_CAS_H
#define KICKOS_ARCH_ATOMIC_CAS_H

#if KICKOS_KERNEL_CORES > 1

#include <kickos/arch/atomic_cas_bounded.h>

#include <stdint.h>

inline bool arch_cas_u32_once(uint32_t* word, uint32_t want, uint32_t desired, uint32_t* observed)
{
    // No MEMW: S32C1I is both acquire and release (Xtensa ISA 4.3.13.5).
    // A failed S32C1I may answer ~SCOMPARE1 rather than the word (Xtensa ISA 8.3.281), so the
    // answer on failure is a fresh load.
    uint32_t got = 0;
    uint32_t stored = 0;
    __asm volatile("    wsr.scompare1 %[want]\n\t"
                   "    mov %[got], %[val]\n\t"
                   "    s32c1i %[got], %[addr], 0\n\t"
                   "    movi %[ok], 1\n\t"
                   "    beq %[got], %[want], 1f\n\t"
                   "    movi %[ok], 0\n\t"
                   "    l32ai %[got], %[addr], 0\n\t"
                   "1:"
                   : [got] "=&a"(got), [ok] "=&a"(stored)
                   : [addr] "a"(word), [want] "a"(want), [val] "a"(desired)
                   : "memory");
    *observed = got;
    return stored != 0;
}

#else

#include <kickos/arch/atomic_cas_masked.h>

#endif

#endif
