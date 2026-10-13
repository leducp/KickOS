// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350's membership of the RP2xxx family (../rp2xxx/rp2xxx.h). Read by
// chip_rp2xxx.cc.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_FAMILY_MAP_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_FAMILY_MAP_H

// Opened empty so the alias below can name it before any regs/ header has.
namespace kickos::rp2350::reg
{
}

namespace kickos::rp2xxx
{
    namespace reg = kickos::rp2350::reg;
}

#include <kickos/arch/bounded_wait.h>
#include <kickos/io/mmio.h>

#include "regs/clocks.h"
#include "regs/pads.h"
#include "regs/ticks.h"
#include "regs/xosc.h"

#include <stdint.h>

namespace kickos::rp2xxx::part
{
    // The pads reset isolated and stay disconnected until ISO clears.
    constexpr uint32_t PAD_RELEASE = reg::pads::ISO;
    static_assert(PAD_RELEASE == (1u << 8), "the RP2350 pad's isolation latch is bit 8");

    // clk_ref is the source AFTER CLK_REF_DIV (datasheet 8.1), which the bootrom overwrites.
    inline uint32_t clk_ref_xosc_hz()
    {
        uint32_t div = (r32(reg::clocks::CLK_REF_DIV) & reg::clocks::CLK_REF_DIV_INT_MASK)
                       >> reg::clocks::CLK_REF_DIV_INT_SHIFT;
        if (div == 0u)
        {
            div = reg::clocks::CLK_REF_DIV_INT_ZERO;
        }
        return reg::xosc::FREQ_HZ / div;
    }

    // The generator must be stopped before CYCLES changes (datasheet 8.5.1).
    inline void tick_start(uint32_t cycles)
    {
        r32(reg::ticks::TIMER0_CTRL) = 0;
        r32(reg::ticks::TIMER0_CYCLES) = cycles;
        r32(reg::ticks::TIMER0_CTRL) = reg::ticks::CTRL_ENABLE;
    }

    inline void clk_usb_stop_wait()
    {
        (void)kickos::arch::wait_clear(r32(reg::clocks::CLK_USB_CTRL), reg::clocks::CLK_USB_ENABLED);
    }
}

#endif
