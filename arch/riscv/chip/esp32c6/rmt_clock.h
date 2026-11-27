// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RMT's gates in kernel-owned PCR: what arch_periph_enable sets for the RMT's holder, and what
// the diagnostic LED sets at the panic.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_RMT_CLOCK_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_RMT_CLOCK_H

#include <kickos/io/mmio.h>

#include <stdint.h>

#include "regs/pcr.h"

namespace kickos::esp32c6::rmt_clock
{
    // The bus clock on and the reset released, then the function clock from XTAL (SCLK_SEL 3,
    // 40 MHz) undivided (DIV_NUM 0). Asserts no reset, so a running block keeps its state.
    inline void enable()
    {
        r32(reg::pcr::RMT_CONF) = (r32(reg::pcr::RMT_CONF) | reg::pcr::RMT_CLK_EN) & ~reg::pcr::RMT_RST_EN;
        r32(reg::pcr::RMT_SCLK_CONF) =
            (3u << reg::pcr::RMT_SCLK_SEL_S) | (0u << reg::pcr::RMT_SCLK_DIV_NUM_S) | reg::pcr::RMT_SCLK_EN;
    }
}

#endif
