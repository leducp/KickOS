// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Generic volatile MMIO accessors, static inline so a caller gets a bare
// volatile load/store at the poke site (no call). Shared by the userspace device
// drivers that reach a granted register window: r8/r16/r32 return an lvalue
// reference to the width-typed volatile at `addr`, so `r32(base + OFF) = v` and
// `r32(base + OFF) |= bit` read exactly as the hand-written cast did.

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
