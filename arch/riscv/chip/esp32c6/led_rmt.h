// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A WS2812B pixel driven through RMT channel 0: the bodies of arch_diag_led_init and
// arch_diag_led_set on a board whose diagnostic LED is addressable. GPIO bit-banging cannot meet
// the ~400 ns high time; this sends one polled frame with no interrupt and no DMA.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_LED_RMT_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_LED_RMT_H

#include <kickos/io/mmio.h>

#include <stdint.h>

#include "regs/gpio.h"
#include "regs/io_mux.h"
#include "regs/pcr.h"
#include "regs/rmt.h"
#include "rmt_clock.h"

namespace kickos::esp32c6::led_rmt
{
    // Pulse widths in RMT ticks: XTAL 40 MHz / group 1 / div_cnt 2 = 50 ns a tick, every bit
    // period 1.25 us, all within WS2812B tolerance.
    constexpr uint32_t DIV_CNT = 2;
    constexpr uint32_t T0H = 8;      // 400 ns
    constexpr uint32_t T0L = 17;     // 850 ns
    constexpr uint32_t T1H = 16;     // 800 ns
    constexpr uint32_t T1L = 9;      // 450 ns
    constexpr uint32_t RESET = 1200; // 60 us low latch, then the end marker
    constexpr uint32_t BITS = 24;

    // CH0CONF0's R/W part (its write-triggered bits held 0): div_cnt, 1 RAM block, idle driven low.
    constexpr uint32_t CH0_CFG =
        (DIV_CNT << reg::rmt::DIV_CNT_S) | (1u << reg::rmt::MEM_SIZE_S) | reg::rmt::IDLE_OUT_EN;

    // A RAM word holds two {duration:15, level:1} pulses: a bit is a high pulse then a low one.
    constexpr uint32_t word(uint32_t high, uint32_t low)
    {
        return high | (1u << 15) | (low << 16);
    }

    // Takes the block back from whatever a task left it in: its clock, its reset, then channel 0
    // and `pin`'s route to it. The reset ends every channel's transmission.
    inline void init(uint32_t pin)
    {
        r32(reg::pcr::RMT_CONF) = r32(reg::pcr::RMT_CONF) | reg::pcr::RMT_CLK_EN;
        r32(reg::pcr::RMT_CONF) = r32(reg::pcr::RMT_CONF) | reg::pcr::RMT_RST_EN;
        rmt_clock::enable();
        r32(reg::rmt::SYS_CONF) = r32(reg::rmt::SYS_CONF) | reg::rmt::APB_FIFO_MASK;
        r32(reg::rmt::CH0CONF0) = CH0_CFG; // carrier_en resets to 1
        r32(reg::rmt::CH0CONF0) = CH0_CFG | reg::rmt::CONF_UPDATE;

        r32(reg::gpio::func_out_sel_cfg(pin)) = reg::gpio::RMT_SIG_OUT0_IDX;
        r32(reg::gpio::ENABLE_W1TS) = 1u << pin;
        r32(reg::io_mux::gpio(pin)) = reg::io_mux::MCU_SEL_GPIO | reg::io_mux::FUN_DRV_2;
    }

    // One 24-bit frame, most significant bit first. Bounded, so a wedged RMT never hangs the
    // fault path.
    inline void send(uint32_t color)
    {
        for (uint32_t i = 0; i < BITS; i++)
        {
            uint32_t w = word(T0H, T0L);
            if (((color >> (BITS - 1u - i)) & 1u) != 0u)
            {
                w = word(T1H, T1L);
            }
            r32(reg::rmt::CH0_RAM + 4u * i) = w;
        }
        r32(reg::rmt::CH0_RAM + 4u * BITS) = RESET; // pulse1 {0,0} is the end marker

        r32(reg::rmt::INT_CLR) = reg::rmt::CH0_TX_END;
        r32(reg::rmt::CH0CONF0) = CH0_CFG | reg::rmt::MEM_RD_RST | reg::rmt::APB_MEM_RST;
        r32(reg::rmt::CH0CONF0) = CH0_CFG;
        r32(reg::rmt::CH0CONF0) = CH0_CFG | reg::rmt::CONF_UPDATE;
        r32(reg::rmt::CH0CONF0) = CH0_CFG | reg::rmt::TX_START;

        uint32_t spin = 0;
        while ((r32(reg::rmt::INT_RAW) & reg::rmt::CH0_TX_END) == 0u)
        {
            spin++;
            if (spin > 2000000u)
            {
                break;
            }
        }
    }
}

#endif
