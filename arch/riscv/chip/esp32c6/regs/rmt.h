// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 RMT registers (TRM v1.2 ch.30). Drives the onboard WS2812B diag LED
// (GPIO8): GPIO bit-bang cannot meet the ~400 ns bit high-time even at 160 MHz, so
// the RMT clocks the pulse train in hardware. Channel 0 (TX). The channel clock is
// muxed/divided in PCR on the C6 (see regs/pcr.h), NOT in RMT_SYS_CONF.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_RMT_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_RMT_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32c6::reg::rmt
{
    constexpr uintptr_t CH0CONF0 = mmap::RMT_BASE + 0x10u; // one config reg per TX channel
    constexpr uintptr_t INT_RAW = mmap::RMT_BASE + 0x38u;
    constexpr uintptr_t INT_CLR = mmap::RMT_BASE + 0x44u;
    constexpr uintptr_t SYS_CONF = mmap::RMT_BASE + 0x68u;
    constexpr uintptr_t CH0_RAM = mmap::RMT_BASE + 0x400u; // 48-word (192 B) channel-0 block

    // CH0CONF0 bits. tx_start/mem_rd_rst/apb_mem_rst/conf_update are write-triggered
    // (self-clearing); div_cnt/mem_size/idle/carrier are R/W and need conf_update to latch.
    constexpr uint32_t TX_START = 1u << 0;
    constexpr uint32_t MEM_RD_RST = 1u << 1;
    constexpr uint32_t APB_MEM_RST = 1u << 2;
    constexpr uint32_t IDLE_OUT_EN = 1u << 6;  // drive idle level (idle_out_lv=0 -> rest low)
    constexpr uint32_t CARRIER_EN = 1u << 21;  // default 1 -- MUST clear (no IR carrier)
    constexpr uint32_t CONF_UPDATE = 1u << 24;
    constexpr uint32_t DIV_CNT_S = 8u;         // [15:8] per-channel clock divider
    constexpr uint32_t MEM_SIZE_S = 16u;       // [18:16] RAM blocks

    constexpr uint32_t CH0_TX_END = 1u << 0;    // INT_RAW/INT_CLR bit0: TX done
    constexpr uint32_t APB_FIFO_MASK = 1u << 0; // SYS_CONF: 1 = access RAM directly (not FIFO)
}

#endif
