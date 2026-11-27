// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The AIPSTZ slot a held window's block sits in, opened and closed for its holder: the bodies of
// arch_periph_enable and arch_periph_disable, over the register accessor the chip passes.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_AIPSTZ_GATE_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_AIPSTZ_GATE_H

#include <kickos/sys/errno.h>

#include <stdint.h>

#include "regs/aipstz.h"

namespace kickos::imxrt1062::aipstz_gate
{
    using Reg = volatile uint32_t& (*)(uintptr_t address);

    // The bases whose slot a holder may have opened. Exact base match, never a range: a base earns
    // an entry only once its device is the whole 16 KiB AIPS slot or the rest of that slot is
    // reserved in arch_reserved_blocks.
    constexpr uintptr_t GATED[] = {
        mmap::USB1_BASE,     mmap::LPUART1_BASE, mmap::LPUART2_BASE, mmap::LPUART3_BASE,
        mmap::LPUART4_BASE, mmap::LPUART5_BASE, mmap::LPUART6_BASE, mmap::LPUART7_BASE,
        mmap::LPUART8_BASE,
    };

    // The OPACR word holding `base`'s slot, or 0 for a base with no entry.
    inline uintptr_t word_of(uintptr_t base)
    {
        bool gated = false;
        for (uintptr_t const g : GATED)
        {
            if (g == base)
            {
                gated = true;
            }
        }
        if (not gated)
        {
            return 0u;
        }
        uintptr_t const bridge = reg::aipstz::bridge_of(base);
        if (bridge == 0u)
        {
            return 0u; // outside all four AIPS regions: refuse rather than pick a word
        }
        return reg::aipstz::opacr_of(bridge, reg::aipstz::slot_of(base));
    }

    // Clears the slot's Supervisor-Protect bit.
    inline int open(uintptr_t base, Reg r)
    {
        uintptr_t const word = word_of(base);
        if (word == 0u)
        {
            return -KOS_EINVAL;
        }
        r(word) = r(word) & ~reg::aipstz::sp_bit_of(reg::aipstz::slot_of(base));
        return 0;
    }

    // Sets the slot's Supervisor-Protect bit back.
    inline int close(uintptr_t base, Reg r)
    {
        uintptr_t const word = word_of(base);
        if (word == 0u)
        {
            return -KOS_EINVAL;
        }
        r(word) = r(word) | reg::aipstz::sp_bit_of(reg::aipstz::slot_of(base));
        return 0;
    }
}

#endif
