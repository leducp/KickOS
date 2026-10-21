// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/user_cas.h>

extern "C" uint32_t kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired)
{
    __asm__ volatile("lock cmpxchgl %2, %1"
                     : "+a"(expected), "+m"(*word)
                     : "r"(desired)
                     : "cc", "memory");
    return expected;
}
