// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 interrupt matrix (INTMTX) registers (TRM v1.2 section 1.6 + ch.10,
// offsets from Table 10.4.2). Each peripheral source has a MAP register whose [4:0]
// selects the target CPU interrupt ID (then configured in the PLIC).

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTMTX_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_INTMTX_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32c6::reg::intmtx
{
    constexpr uintptr_t FROM_CPU_0_MAP = mmap::INTMTX_BASE + 0x58u; // [4:0]=target CPU int
    constexpr uintptr_t UART0_MAP = mmap::INTMTX_BASE + 0xACu;      // route UART0 -> CPU int
}

#endif
