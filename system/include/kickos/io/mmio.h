// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Volatile MMIO accessors for the userspace drivers that reach a granted register window.
// The lvalue reference is what allows `r32(base + OFF) = v` and `r32(base + OFF) |= bit`;
// static inline keeps the poke site a bare volatile load/store with no call.

#ifndef KICKOS_IO_MMIO_H
#define KICKOS_IO_MMIO_H

#include <stdint.h>

static inline volatile uint8_t& r8(uintptr_t addr)
{
    return *reinterpret_cast<volatile uint8_t*>(addr);
}

static inline volatile uint16_t& r16(uintptr_t addr)
{
    return *reinterpret_cast<volatile uint16_t*>(addr);
}

static inline volatile uint32_t& r32(uintptr_t addr)
{
    return *reinterpret_cast<volatile uint32_t*>(addr);
}

#endif
