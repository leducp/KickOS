// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ATOMIC_CAS_H
#define KICKOS_ARCH_ATOMIC_CAS_H

#include <kickos/arch/atomic_cas_bounded.h>

#include <stdint.h>

inline bool arch_cas_u32_once(uint32_t* word, uint32_t want, uint32_t desired, uint32_t* observed)
{
    // lr.w sign-extends what it loads, so the operand it is compared with must be too.
    int64_t const want64 = static_cast<int32_t>(want);
    int64_t got = 0;
    int64_t failed = 0;
    __asm volatile("    li      %1, 1\n"
                   "    lr.w.aqrl %0, (%2)\n"
                   "    bne     %0, %3, 1f\n"
                   "    sc.w.rl %1, %4, (%2)\n"
                   "1:"
                   : "=&r"(got), "=&r"(failed)
                   : "r"(word), "r"(want64), "r"(desired)
                   : "memory");
    *observed = static_cast<uint32_t>(got);
    return failed == 0;
}

#endif
