// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 GPIO matrix registers (TRM v1.2 ch.7). Block 0x6009_1000..0x6009_1FFF.
// The W1TS/W1TC atomic set/clear pair is the per-thread MMIO isolation window used by
// the c6blink PMP proof (8 B at 0x6009_1008). GPIO_OUT_REG (+0x04) is also owned by
// the shared class-driver leaf (class/gpio_class.h).

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_GPIO_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_GPIO_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32c6::reg::gpio
{
    constexpr uintptr_t BLOCK_END = mmap::GPIO_BASE + 0xFFFu; // last byte of the 4 KB block

    // Base-relative offsets (consumed by the freestanding class leaf, which takes the
    // block base as a parameter). The address-form constants below derive from these.
    constexpr uintptr_t OUT_OFFSET = 0x04u;         // Reg 7.1 output data latch
    constexpr uintptr_t OUT_W1TS_OFFSET = 0x08u;    // Reg 7.2 set-1s (WT)
    constexpr uintptr_t OUT_W1TC_OFFSET = 0x0Cu;    // Reg 7.3 clear-1s (WT)
    constexpr uintptr_t ENABLE_OFFSET = 0x20u;      // Reg 7.4 direction
    constexpr uintptr_t ENABLE_W1TS_OFFSET = 0x24u;
    constexpr uintptr_t FUNC_OUT_SEL_CFG_BASE_OFFSET = 0x554u; // FUNCn @ +0x4*n

    constexpr uintptr_t OUT = mmap::GPIO_BASE + OUT_OFFSET;
    constexpr uintptr_t OUT_W1TS = mmap::GPIO_BASE + OUT_W1TS_OFFSET;
    constexpr uintptr_t OUT_W1TC = mmap::GPIO_BASE + OUT_W1TC_OFFSET;
    constexpr uintptr_t ENABLE = mmap::GPIO_BASE + ENABLE_OFFSET;
    constexpr uintptr_t ENABLE_W1TS = mmap::GPIO_BASE + ENABLE_W1TS_OFFSET;
    constexpr uintptr_t FUNC_OUT_SEL_CFG_BASE = mmap::GPIO_BASE + FUNC_OUT_SEL_CFG_BASE_OFFSET;

    // Per-pin output-function select register (FUNCn_OUT_SEL_CFG).
    inline constexpr uintptr_t func_out_sel_cfg(uint32_t pin)
    {
        return FUNC_OUT_SEL_CFG_BASE + 0x4u * pin;
    }

    constexpr uint32_t OUT_SEL_SIMPLE = 128u;    // out-sel value: bit n of GPIO_OUT drives the pad
    constexpr uint32_t RMT_SIG_OUT0_IDX = 71u;   // GPIO matrix signal index: RMT ch-0 TX
}

#endif
