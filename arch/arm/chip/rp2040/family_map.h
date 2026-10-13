// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2040's membership of the RP2xxx family (../rp2xxx/rp2xxx.h). Read by
// chip_rp2xxx.cc.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_FAMILY_MAP_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_FAMILY_MAP_H

// Opened empty so the alias below can name it before any regs/ header has.
namespace kickos::rp2040::reg
{
}

namespace kickos::rp2xxx
{
    namespace reg = kickos::rp2040::reg;
}

#include <kickos/io/mmio.h>

#include "regs/watchdog.h"
#include "regs/xosc.h"

#include <stdint.h>

namespace kickos::rp2xxx::part
{
    constexpr uint32_t PAD_RELEASE = 0u;
    static_assert(PAD_RELEASE == 0u, "the RP2040 pad register has no isolation bit to release");

    inline uint32_t clk_ref_xosc_hz()
    {
        return reg::xosc::FREQ_HZ;
    }

    inline void tick_start(uint32_t cycles)
    {
        r32(reg::watchdog::TICK) = reg::watchdog::TICK_ENABLE | cycles;
    }

    // clk_usb has no CTRL.ENABLED bit here, so its stop is a timed wait: the datasheet rule is two
    // cycles of the source (2.15.3.2), and this spin covers that at any clk_sys the board reaches.
    inline void clk_usb_stop_wait()
    {
        for (uint32_t i = 0; i < 64u; i++)
        {
            __asm volatile("nop");
        }
    }
}

#endif
