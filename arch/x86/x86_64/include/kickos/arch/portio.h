// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_PORTIO_H
#define KICKOS_ARCH_PORTIO_H

#include <stdint.h>

namespace kickos::x86_64
{
    inline void outb(uint16_t port, uint8_t value)
    {
        __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
    }

    inline uint8_t inb(uint16_t port)
    {
        uint8_t value = 0;
        __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }
}

#endif
