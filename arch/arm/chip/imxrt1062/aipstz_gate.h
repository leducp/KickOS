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

    // The OPACR word holding `base`'s slot, or 0 for a base with no entry. Exact base match,
    // never a range: a base earns an entry only once the rest of its 16 KiB AIPS slot is
    // reserved in arch_reserved_blocks.
    inline uintptr_t word_of(uintptr_t base)
    {
        if (base != mmap::USB1_BASE)
        {
            return 0u;
        }
        uintptr_t const bridge = reg::aipstz::bridge_of(base);
        if (bridge == 0u)
        {
            return 0u; // outside all four AIPS regions: refuse rather than pick a word
        }
        return reg::aipstz::opacr_of(bridge, reg::aipstz::USB1_SLOT);
    }

    // Clears the slot's Supervisor-Protect bit.
    inline int open(uintptr_t base, Reg r)
    {
        uintptr_t const word = word_of(base);
        if (word == 0u)
        {
            return -KOS_EINVAL;
        }
        r(word) = r(word) & ~reg::aipstz::sp_bit_of(reg::aipstz::USB1_SLOT);
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
        r(word) = r(word) | reg::aipstz::sp_bit_of(reg::aipstz::USB1_SLOT);
        return 0;
    }
}

#endif
