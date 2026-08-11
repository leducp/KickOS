// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 PCR (Power/Clock/Reset) registers (TRM v1.2 ch.8, offsets from section 8.5.1).
// On this core the peripheral clock source select + divider live here, not in the
// peripheral's own SYS_CONF, and PCR is an owns-for-life reserved MPU block.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_PCR_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_PCR_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::pcr
{
    constexpr uintptr_t UART0_SCLK_CONF = mmap::PCR_BASE + 0x04u; // Register 8.2
    constexpr uintptr_t RMT_CONF = mmap::PCR_BASE + 0x2Cu;        // CLK_EN bit0, RST_EN bit1
    constexpr uintptr_t RMT_SCLK_CONF = mmap::PCR_BASE + 0x30u;
    constexpr uintptr_t SYSCLK_CONF = mmap::PCR_BASE + 0x110u; // Register 8.63

    constexpr uint32_t RMT_CLK_EN = 1u << 0;
    constexpr uint32_t RMT_RST_EN = 1u << 1;
    constexpr uint32_t RMT_SCLK_EN = 1u << 22;
    constexpr uint32_t RMT_SCLK_SEL_S = 20u;     // [21:20] 1=PLL_F80M 2=RC_FAST 3=XTAL
    constexpr uint32_t RMT_SCLK_DIV_NUM_S = 12u; // [19:12] group divisor = value+1

    // UART0 function clock (Register 8.2).
    constexpr uint32_t UART0_SCLK_DIV_A_S = 0u;
    constexpr uint32_t UART0_SCLK_DIV_A_MASK = 0x3Fu;
    constexpr uint32_t UART0_SCLK_DIV_B_S = 6u;
    constexpr uint32_t UART0_SCLK_DIV_B_MASK = 0x3Fu;
    constexpr uint32_t UART0_SCLK_DIV_NUM_S = 12u;
    constexpr uint32_t UART0_SCLK_DIV_NUM_MASK = 0xFFu;
    constexpr uint32_t UART0_SCLK_SEL_S = 20u;
    constexpr uint32_t UART0_SCLK_SEL_MASK = 0x3u;
    constexpr uint32_t UART0_SCLK_EN = 1u << 22;
    constexpr uint32_t SCLK_SEL_NONE = 0u;
    constexpr uint32_t SCLK_SEL_PLL_F80M = 1u;
    constexpr uint32_t SCLK_SEL_RC_FAST = 2u;
    constexpr uint32_t SCLK_SEL_XTAL = 3u; // the reset selection

    // SYSCLK_CONF (Register 8.63). CLK_XTAL_FREQ is RO and reports the crystal's frequency
    // in MHz.
    constexpr uint32_t CLK_XTAL_FREQ_S = 24u; // [30:24]
    constexpr uint32_t CLK_XTAL_FREQ_MASK = 0x7Fu;

    // PLL_F80M is the SPLL/6 tap and the TRM names it at 80 MHz (Register 8.68). SPLL's own
    // rate lives in PCR_PLL_FREQ, which is HRO: "Only hardware can read from this
    // register/field" (TRM Access Types glossary), so it has no software read-back.
    constexpr uint32_t PLL_F80M_HZ = 80000000u;
}

#endif
