// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ATOMIC_CAS_H
#define KICKOS_ARCH_ATOMIC_CAS_H

#include <kickos/arch/atomic_cas_bounded.h>

#include <stdint.h>

// Inline, never a builtin: that compiles to a call of libgcc's outline-atomic helper, which is
// app text the kernel may not reach.
inline bool arch_cas_u32_once(uint32_t* word, uint32_t want, uint32_t desired, uint32_t* observed)
{
    uint32_t got = 0;
    uint32_t failed = 0;
    __asm volatile("    mov     %w1, #1\n"
                   "    ldaxr   %w0, [%2]\n"
                   "    cmp     %w0, %w3\n"
                   "    b.ne    1f\n"
                   "    stlxr   %w1, %w4, [%2]\n"
                   "    b       2f\n"
                   "1:  clrex\n"
                   "2:"
                   : "=&r"(got), "=&r"(failed)
                   : "r"(word), "r"(want), "r"(desired)
                   : "cc", "memory");
    *observed = got;
    return failed == 0;
}

#endif
