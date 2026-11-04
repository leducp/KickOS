// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_COMMON_CRT_TAIL_H
#define KICKOS_ARCH_COMMON_CRT_TAIL_H

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();
    extern uint32_t __kickos_image0_data_load[];
    extern uint32_t __kickos_image0_data_start[];
    extern uint32_t __kickos_image0_data_end[];
    extern uint32_t __kickos_image0_window_end[];
    extern uint32_t __kickos_image1_data_load[];
    extern uint32_t __kickos_image1_data_start[];
    extern uint32_t __kickos_image1_data_end[];
    extern uint32_t __kickos_image1_window_end[];
}

// An image's .data from its load image, then the rest of its window zeroed.
inline __attribute__((always_inline)) void kickos_crt_image_data(uint32_t const* src,
                                                                 uint32_t* data,
                                                                 uint32_t* data_end,
                                                                 uint32_t* end)
{
    for (uint32_t* dst = data; dst < data_end; dst++)
    {
        *dst = *src++;
    }
    for (uint32_t* b = data_end; b < end; b++)
    {
        *b = 0;
    }
}

// Every image's, on a chip whose Reset_Handler copies the kernel's own ranges itself.
inline __attribute__((always_inline)) void kickos_crt_images_data(void)
{
    kickos_crt_image_data(__kickos_image0_data_load, __kickos_image0_data_start,
                          __kickos_image0_data_end, __kickos_image0_window_end);
    kickos_crt_image_data(__kickos_image1_data_load, __kickos_image1_data_start,
                          __kickos_image1_data_end, __kickos_image1_window_end);
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
