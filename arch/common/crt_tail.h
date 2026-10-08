// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_COMMON_CRT_TAIL_H
#define KICKOS_ARCH_COMMON_CRT_TAIL_H

#include <kickos/arch/arch.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();
}

// ALWAYS inlined: the image rules read the constructor loop as part of Reset_Handler's own body.
inline __attribute__((always_inline)) void kickos_crt_ctors(void)
{
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
}

inline __attribute__((always_inline)) void kickos_crt_tail(void)
{
    kickos_crt_ctors();
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

#endif
