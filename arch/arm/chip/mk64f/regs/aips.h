// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 AIPS0 peripheral-bridge register map (K64 Sub-Family RM ch.20): the
// per-4-KB-slot access gate (MPRA + PACRA..PACRP).

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_AIPS_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_AIPS_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::mk64f::reg::aips
{
    // AIPS0 fronts 0x4000_0000..0x4007_FFFF as 128 x 4 KB slots (RM 4.5.2 bridge map).
    constexpr uint32_t SLOT_STRIDE = 0x1000u;

    // PACR offsets are NOT contiguous: PACRA..PACRD (slots 0..31) at 0x20..0x2C, then
    // 0x30..0x3C reserved, PACRE..PACRP (slots 32..127) at 0x40..0x6C (RM 20.2.3).
    constexpr uintptr_t PACRA = mmap::AIPS0_BASE + 0x20u;
    constexpr uintptr_t PACRE = mmap::AIPS0_BASE + 0x40u;
    constexpr uint32_t PACR_LOW_GROUPS = 4u; // groups covered by PACRA..PACRD
    constexpr uint32_t SLOT_COUNT = 128u;    // slots with a PACR field on THIS bridge

    // No slot: the base is outside AIPS0. Callers must reject it before pacr_of, whose
    // domain is a slot below SLOT_COUNT; an AIPS1 base would otherwise derive an address
    // past the PACRP ceiling at +0x6C (UART4_BASE -> slot 234 -> 0x400000A4).
    constexpr uint32_t SLOT_NONE = 0xFFFFFFFFu;

    constexpr uint32_t slot_of(uintptr_t base)
    {
        if (base < mmap::AIPS0_BASE or base >= mmap::AIPS1_BASE)
        {
            return SLOT_NONE;
        }
        return static_cast<uint32_t>((base - mmap::AIPS0_BASE) / SLOT_STRIDE);
    }

    // PACR holding slot's 4-bit field; 8 slots per register. slot < SLOT_COUNT.
    constexpr uintptr_t pacr_of(uint32_t slot)
    {
        uint32_t const group = slot / 8u;
        if (group < PACR_LOW_GROUPS)
        {
            return PACRA + group * 4u;
        }
        return PACRE + (group - PACR_LOW_GROUPS) * 4u;
    }

    // Field n of a PACR is a nibble, field 0 at bits [31:28] down to field 7 at [3:0];
    // nibble layout is reserved[3]/SP[2]/WP[1]/TP[0], so SP = 28 - 4*(slot%8) + 2
    // (RM 20.2.3). PACRs reset to 0x4444_4444, SP=1 = supervisor-only (RM 3.3.8.4).
    constexpr uint32_t pacr_sp_bit(uint32_t slot)
    {
        return 1u << (30u - 4u * (slot % 8u));
    }

    // Reproduces the known-good values: slot 106 (UART0) = PACRN 0x4000_0064 bit 22,
    // slot 44 (DSPI0) = PACRF 0x4000_0044 bit 14, slot 55 (PIT) = PACRG 0x4000_0048 bit 2.
    static_assert(slot_of(mmap::UART0_BASE) == 106u);
    static_assert(pacr_of(106u) == 0x40000064u);
    static_assert(pacr_sp_bit(106u) == (1u << 22));
    static_assert(slot_of(mmap::DSPI0_BASE) == 44u);
    static_assert(pacr_of(44u) == 0x40000044u);
    static_assert(pacr_sp_bit(44u) == (1u << 14));
    static_assert(slot_of(mmap::PIT_BASE) == 55u);
    static_assert(pacr_of(55u) == 0x40000048u);
    static_assert(pacr_sp_bit(55u) == (1u << 2));

    // The bound itself: the last AIPS0 slot resolves, and the AIPS1 blocks (UART4, GPIOA)
    // do not.
    static_assert(slot_of(mmap::AIPS1_BASE - SLOT_STRIDE) == SLOT_COUNT - 1u);
    static_assert(pacr_of(SLOT_COUNT - 1u) == mmap::AIPS0_BASE + 0x6Cu);
    static_assert(slot_of(mmap::UART4_BASE) == SLOT_NONE);
    static_assert(slot_of(mmap::GPIOA_BASE) == SLOT_NONE);
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_REGS_AIPS_H
