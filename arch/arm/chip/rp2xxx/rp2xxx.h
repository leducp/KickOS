// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two parts are on DIFFERENT ISAs (armv6m and armv7m) and share no arch archive, so
// chip_rp2xxx.cc is compiled into each chip's archive (its family.cmake) against that chip's
// regs/ headers.
//
// A chip joins the family by aliasing its register namespace into kickos::rp2xxx (its
// family_map.h) and by spelling what this unit names: reg::uart::BASE and the
// reg::uart::IBRD_PLL / FBRD_PLL pair.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2XXX_RP2XXX_H
#define KICKOS_ARCH_ARM_CHIP_RP2XXX_RP2XXX_H

#include <stdint.h>

namespace kickos::rp2xxx
{
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // Every APB register is mirrored at these offsets from its own address; SIO is not.
    constexpr uintptr_t ATOMIC_SET = 0x2000u;
    constexpr uintptr_t ATOMIC_CLR = 0x3000u;

    // Far longer than any legitimate wait: reaching it means a dead crystal or stuck peripheral.
    constexpr uint32_t POLL_TIMEOUT = 1000000u;

    bool wait_mask(uintptr_t addr, uint32_t mask);
    void unreset(uint32_t mask);
    bool pll_sys_lock();

#if KICKOS_AMP_OWN_IMAGE
    // Defined by the chip (chip_rp2350.cc): serialises the polled console writer against the
    // peer kernel sharing the UART.
    bool console_claim(void);
    bool console_claim_open(void);
    void console_drop(bool ended_line);
#endif
}

#endif
