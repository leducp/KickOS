// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See aspace_sysops_seam.h.

#include <kickos/arch/arch.h>

#include "aspace_sysops_seam.h"

namespace
{
    using kickos::testfix::cpu;
    using kickos::testfix::sysop_fire_mid_edit;
    using kickos::testfix::sysop_record;

    uint64_t g_ttbr0[KICKOS_NUM_CORES] = {};

    // TCR_EL1 as startup.S programs it: T0SZ 25, so a 39-bit low half whose walk starts at
    // level 1, IPS 0b010 for a 40-bit output, and AS for a 16-bit identifier.
    constexpr uint64_t TCR_AS = 1ull << 36;
    constexpr uint64_t TCR_DEFAULT = 25ull | (2ull << 32) | TCR_AS;
    // ID_AA64MMFR0_EL1 as the A53 reports it: PARange 0b0010 (40 bits), ASIDBits 0b0010 (16),
    // and 0 in each of the three granule fields, whose senses differ (arch_aspace_model).
    constexpr uint64_t MMFR0_DEFAULT = 2ull | (2ull << 4);

    uint64_t g_tcr = TCR_DEFAULT;
    uint64_t g_mmfr0 = MMFR0_DEFAULT;
}

extern "C"
{
    // The array models the physical RAM window; PA is its byte offset.
    alignas(kickos::testfix::SYSOPS_GRANULE) unsigned char
        __kickos_arm64_va_base[kickos::testfix::SYSOPS_FRAMES * kickos::testfix::SYSOPS_GRANULE] =
            {};

    void kickos_arm64_instruction_side_rendezvous(uint32_t peers)
    {
        sysop_record(kickos::testfix::OP_RENDEZVOUS, peers);
    }

    uint64_t kickos_armv8a_read_tcr_el1(void)
    {
        return g_tcr;
    }

    // A fault: no gate here asks the hardware's own walk.
    uint64_t kickos_armv8a_at_read(uint64_t, bool)
    {
        return 1u;
    }

    uint64_t kickos_armv8a_read_mair_el1(void)
    {
        return 0u;
    }

    uint64_t kickos_armv8a_read_mmfr0_el1(void)
    {
        return g_mmfr0;
    }

    uint64_t kickos_armv8a_read_ttbr0_el1(void)
    {
        return g_ttbr0[cpu()];
    }

    void kickos_armv8a_write_ttbr0_el1(uint64_t ttbr)
    {
        g_ttbr0[cpu()] = ttbr;
        sysop_record(kickos::testfix::OP_WRITE_TTBR0, ttbr);
    }

    void kickos_armv8a_dsb_ishst(void)
    {
        sysop_record(kickos::testfix::OP_DSB_ISHST, 0);
    }

    void kickos_armv8a_dsb_ish(void)
    {
        sysop_record(kickos::testfix::OP_DSB_ISH, 0);
    }

    void kickos_armv8a_isb(void)
    {
        sysop_record(kickos::testfix::OP_ISB, 0);
    }

    void kickos_armv8a_tlbi_page_is(uint64_t page)
    {
        sysop_record(kickos::testfix::OP_TLBI_PAGE_IS, page);
        sysop_fire_mid_edit();
    }

    void kickos_armv8a_tlbi_page_local(uint64_t page)
    {
        sysop_record(kickos::testfix::OP_TLBI_PAGE_LOCAL, page);
        sysop_fire_mid_edit();
    }

    void kickos_armv8a_tlbi_all_is(void)
    {
        sysop_record(kickos::testfix::OP_TLBI_ALL_IS, 0);
    }

    void kickos_armv8a_tlbi_all_local(void)
    {
        sysop_record(kickos::testfix::OP_TLBI_ALL_LOCAL, 0);
    }
}

namespace kickos
{
    namespace testfix
    {
        char const* const OP_DSB_ISHST = "dsb ishst";
        char const* const OP_DSB_ISH = "dsb ish";
        char const* const OP_ISB = "isb";
        char const* const OP_TLBI_PAGE_IS = "tlbi vaae1is";
        char const* const OP_TLBI_PAGE_LOCAL = "tlbi vaae1";
        char const* const OP_TLBI_ALL_IS = "tlbi vmalle1is";
        char const* const OP_TLBI_ALL_LOCAL = "tlbi vmalle1";
        char const* const OP_WRITE_TTBR0 = "msr ttbr0_el1";
        char const* const OP_RENDEZVOUS = "instruction_side_rendezvous";

        void set_tcr(uint64_t tcr)
        {
            g_tcr = tcr;
        }

        void set_mmfr0(uint64_t mmfr0)
        {
            g_mmfr0 = mmfr0;
        }

        uint64_t tcr_a53()
        {
            return TCR_DEFAULT;
        }

        uint64_t tcr_a53_without_as()
        {
            return TCR_DEFAULT & ~TCR_AS;
        }

        uint64_t mmfr0_with_asid_bits(uint64_t field)
        {
            return (MMFR0_DEFAULT & ~(0xFull << 4)) | ((field & 0xFull) << 4);
        }

        void sysops_reset()
        {
            sysops_reset_common(1);
            for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
            {
                g_ttbr0[c] = 0;
            }
            g_tcr = TCR_DEFAULT;
            g_mmfr0 = MMFR0_DEFAULT;
        }
    }
}
