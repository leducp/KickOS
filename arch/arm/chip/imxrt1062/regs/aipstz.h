// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 AIPSTZ bridge access control (RM ch.32).
//
// OPACR0..4 reset to 0x4444_4444, so every off-platform peripheral starts
// supervisor-protected and RM 32.8.2 terminates an unprivileged access at the bridge
// instead of letting it reach a register that is mapped and clocked.
//
// The slot-to-OPACR-index geometry below is NOT tabulated in IMXRT1060RM; it is carried
// over from the i.MX53 RM Table 14-7 and checked against the documented AIPS-3 boundaries.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_AIPSTZ_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_AIPSTZ_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::imxrt1062::reg::aipstz
{
    // One OPACR word carries EIGHT slots, one nibble each, slot 8n in the TOP nibble.
    constexpr uintptr_t OPACR0 = 0x40u;
    constexpr uint32_t SLOTS_PER_WORD = 8u;

    // The protect bit within a slot's nibble, derived from the reset value rather than from a
    // field table: OPACR resets to 0x4444_4444, so the bit the reset sets is nibble bit 2.
    constexpr uint32_t SP_IN_NIBBLE = 1u << 2;

    // USB1 (OTG1) is AIPS-3 slot 24: (0x402E_0000 - 0x4020_0000 - 0x80000) / 0x4000.
    constexpr uint32_t USB1_SLOT = 24u;

    // The bridge a base sits behind, from the AIPS region it falls in. Returns 0 for an
    // address outside all four, which the caller must refuse: a wrong bridge would clear a
    // nibble belonging to a different peripheral.
    constexpr uintptr_t bridge_of(uintptr_t base)
    {
        if (base >= 0x40000000u and base < 0x40100000u)
        {
            return mmap::AIPSTZ1_BASE;
        }
        if (base >= 0x40100000u and base < 0x40200000u)
        {
            return mmap::AIPSTZ2_BASE;
        }
        if (base >= 0x40200000u and base < 0x40300000u)
        {
            return mmap::AIPSTZ3_BASE;
        }
        if (base >= 0x40300000u and base < 0x40400000u)
        {
            return mmap::AIPSTZ4_BASE;
        }
        return 0u;
    }

    constexpr uintptr_t opacr_of(uintptr_t bridge, uint32_t slot)
    {
        return bridge + OPACR0 + (slot / SLOTS_PER_WORD) * 4u;
    }

    // Slot 8n occupies bits 31:28, 8n+7 occupies 3:0.
    constexpr uint32_t sp_bit_of(uint32_t slot)
    {
        uint32_t const nibble = 7u - (slot % SLOTS_PER_WORD);
        return SP_IN_NIBBLE << (nibble * 4u);
    }

    static_assert(bridge_of(mmap::USB1_BASE) == mmap::AIPSTZ3_BASE,
                  "USB1 must resolve to the AIPS-3 bridge");
    static_assert(sp_bit_of(0) == 0x40000000u, "slot 8n is the top nibble");
    static_assert(sp_bit_of(7) == 0x00000004u, "slot 8n+7 is the bottom nibble");
}

#endif
