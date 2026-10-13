// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ATOMIC_SWAP_H
#define KICKOS_ARCH_ATOMIC_SWAP_H

#include <stdint.h>

// Stores value at word and answers what it held, in one indivisible step, with acquire and
// release ordering.
inline uint32_t arch_swap_u32(uint32_t* word, uint32_t value)
{
    // L32AI and S32C1I, as kernel_lock_draw: the store lands only where the word still held cur.
    uint32_t cur = 0;
    uint32_t got = 0;
    __asm volatile("1:  l32ai %[cur], %[addr], 0\n\t"
                   "    wsr.scompare1 %[cur]\n\t"
                   "    mov %[got], %[val]\n\t"
                   "    s32c1i %[got], %[addr], 0\n\t"
                   "    bne %[got], %[cur], 1b\n\t"
                   "    memw"
                   : [cur] "=&a"(cur), [got] "=&a"(got)
                   : [addr] "a"(word), [val] "a"(value)
                   : "memory");
    return cur;
}

#endif
