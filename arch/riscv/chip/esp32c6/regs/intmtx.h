// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 interrupt matrix registers. Each source's MAP register [4:0] selects the target CPU
// interrupt ID; writing 0 disables the source.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTMTX_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTMTX_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::intmtx
{
    constexpr uintptr_t PMU_MAP = mmap::INTMTX_BASE + 0x34u;
    constexpr uintptr_t FROM_CPU_0_MAP = mmap::INTMTX_BASE + 0x58u; // [4:0]=target CPU int
    constexpr uintptr_t UART0_MAP = mmap::INTMTX_BASE + 0xACu;      // route UART0 -> CPU int
}

#endif
