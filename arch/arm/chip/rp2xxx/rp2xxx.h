// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bring-up the RP2040 and the RP2350 share, declared for both chip backends.
//
// The two parts are on DIFFERENT ISAs (armv6m and armv7m), so they link no arch archive in
// common and this unit cannot live in one. chip_rp2xxx.cc enters each CHIP archive instead,
// named by that chip's family.cmake, and reads the configured chip's own regs/ headers.
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
    // An APB device's slot: its registers and the three alias images above them.
    constexpr uintptr_t APB_ATOMIC_WINDOW = 0x4000u;

    // Bounded so a dead or missing crystal, or a stuck peripheral, degrades instead of
    // hanging the boot forever. The cap is far longer than any legitimate wait.
    constexpr uint32_t POLL_TIMEOUT = 1000000u;

    bool wait_mask(uintptr_t addr, uint32_t mask);
    void unreset(uint32_t mask);
    bool pll_sys_lock();

#if KICKOS_AMP_OWN_IMAGE
    // One UART and two kernels: the chip owning a partition posture serialises the polled
    // writer against its peer. Defined by that chip, not here, the claim being its own
    // hardware's (chip_rp2350.cc).
    bool console_claim(void);
    bool console_claim_open(void);
    void console_drop(bool ended_line);
#endif
}

#endif
