// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See aspace_sysops_seam.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kickos/arch/arch.h>

#include "aspace_sysops_seam.h"

extern "C"
{
    extern unsigned char __kickos_arm64_va_base[];
    void kickos_armv8a_direct_map_record(void);
}

namespace
{
    using kickos::testfix::cpu;
    using kickos::testfix::sysop_fire_mid_edit;
    using kickos::testfix::sysop_record;

    uint64_t g_ttbr0[KICKOS_NUM_CORES] = {};
    uint64_t g_user_ttbr0[KICKOS_NUM_CORES] = {};
    bool g_deferred = false;

    // TCR_EL1 as startup.S programs it: T0SZ and T1SZ 25, so 39-bit halves whose walks start at
    // level 1, IPS 0b010 for a 40-bit output, and AS for a 16-bit identifier.
    constexpr uint64_t TCR_AS = 1ull << 36;
    constexpr uint64_t TCR_DEFAULT = 25ull | (25ull << 16) | (2ull << 32) | TCR_AS;
    // ID_AA64MMFR0_EL1 as the A53 reports it: PARange 0b0010 (40 bits), ASIDBits 0b0010 (16),
    // and 0 in each of the three granule fields, whose senses differ.
    constexpr uint64_t MMFR0_DEFAULT = 2ull | (2ull << 4);

    uint64_t g_tcr = TCR_DEFAULT;
    uint64_t g_mmfr0 = MMFR0_DEFAULT;
    uint32_t g_freed_at_sweep = 0;

    // TTBR1's tables take the RAM window's first frames, which the pool never hands out; the
    // root is frame 0. The most the gates build: the root, one level-2 table for each of the five
    // regions they map in, and five level-3 tables.
    constexpr size_t TTBR1_TABLES = 11;
    constexpr uint64_t TTBR1_ROOT = 0;
    constexpr uint64_t DESC_VALID_TABLE = 3;
    constexpr uint64_t DESC_AF = 1ull << 10;
    constexpr uint64_t DESC_OA = 0x0000FFFFFFFFF000ull;
    constexpr size_t PTES = kickos::testfix::SYSOPS_GRANULE / sizeof(uint64_t);
    size_t g_ttbr1_tables = 1;

    uint64_t* table_at(uint64_t pa)
    {
        return reinterpret_cast<uint64_t*>(&__kickos_arm64_va_base[pa]);
    }

    // The low 39 bits of where the direct map puts `pa`, which is what TTBR1's index path names.
    uint64_t direct_va(uint64_t pa)
    {
        uint64_t const va = reinterpret_cast<uintptr_t>(__kickos_arm64_va_base) + pa;
        return va & ((1ull << 39) - 1u);
    }

    // The table under `parent[index]`, made from the next reserved frame if absent.
    uint64_t* ttbr1_child(uint64_t* parent, size_t index)
    {
        if ((parent[index] & DESC_VALID_TABLE) != DESC_VALID_TABLE)
        {
            if (g_ttbr1_tables == TTBR1_TABLES)
            {
                return nullptr;
            }
            uint64_t const pa = g_ttbr1_tables * kickos::testfix::SYSOPS_GRANULE;
            g_ttbr1_tables++;
            memset(table_at(pa), 0, kickos::testfix::SYSOPS_GRANULE);
            parent[index] = pa | DESC_VALID_TABLE;
        }
        return table_at(parent[index] & DESC_OA);
    }

    size_t index_at(uint64_t va, int level)
    {
        return static_cast<size_t>((va >> (12u + 9u * static_cast<unsigned>(3 - level))) & (PTES - 1u));
    }
}
extern "C"
{
    // The array models the physical RAM window; PA is its byte offset. Aligned to a level-2
    // block, so a block's direct-map address can be as aligned as its frame.
    alignas(0x200000) unsigned char
        __kickos_arm64_va_base[kickos::testfix::SYSOPS_FRAMES * kickos::testfix::SYSOPS_GRANULE] =
            {};

    void kickos_arm64_instruction_side_rendezvous(uint32_t peers)
    {
        sysop_record(kickos::testfix::OP_RENDEZVOUS, peers);
    }

    bool kickos_armv8a_root_deferred(void)
    {
        return g_deferred;
    }

    uint64_t* kickos_armv8a_user_ttbr0_cell(void)
    {
        return &g_user_ttbr0[cpu()];
    }

    uint64_t kickos_armv8a_read_tcr_el1(void)
    {
        return g_tcr;
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

    uint64_t kickos_armv8a_read_ttbr1_el1(void)
    {
        return TTBR1_ROOT;
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
        g_freed_at_sweep = kickos::testfix::frames_freed();
        sysop_record(kickos::testfix::OP_TLBI_ALL_IS, 0);
    }

    void kickos_armv8a_tlbi_all_local(void)
    {
        g_freed_at_sweep = kickos::testfix::frames_freed();
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

        void set_root_deferred(bool on)
        {
            g_deferred = on;
        }

        uint64_t user_ttbr0_of(uint32_t core)
        {
            return g_user_ttbr0[core];
        }

        void set_user_ttbr0(uint32_t core, uint64_t ttbr)
        {
            g_user_ttbr0[core] = ttbr;
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
            sysops_reset_common(TTBR1_TABLES);
            for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
            {
                g_ttbr0[c] = 0;
                g_user_ttbr0[c] = 0;
            }
            g_tcr = TCR_DEFAULT;
            g_mmfr0 = MMFR0_DEFAULT;
            g_freed_at_sweep = 0;
            g_deferred = false;
            ttbr1_reset();
            bool mapped = ttbr1_map(0, 0, SYSOPS_FRAMES, 0);
            for (TtbrRun const& r : TTBR1_GATE_RUNS)
            {
                mapped = mapped and ttbr1_map(r.pa, r.pa, r.pages, 0);
            }
            if (not mapped)
            {
                fprintf(stderr, "FIXTURE FAIL: the seam's TTBR1 table frames ran out\n");
                exit(1);
            }
            ttbr1_record();
        }

        void ttbr1_reset()
        {
            g_ttbr1_tables = 1;
            memset(table_at(TTBR1_ROOT), 0, SYSOPS_GRANULE);
        }

        bool ttbr1_map(arch_phys_addr_t at, arch_phys_addr_t pa, size_t pages, uint64_t attrs)
        {
            for (size_t i = 0; i < pages; i++)
            {
                uint64_t const va = direct_va(at + i * SYSOPS_GRANULE);
                uint64_t* const l2 = ttbr1_child(table_at(TTBR1_ROOT), index_at(va, 1));
                if (l2 == nullptr)
                {
                    return false;
                }
                uint64_t* const l3 = ttbr1_child(l2, index_at(va, 2));
                if (l3 == nullptr)
                {
                    return false;
                }
                l3[index_at(va, 3)] = (pa + i * SYSOPS_GRANULE) | DESC_VALID_TABLE | DESC_AF | attrs;
            }
            return true;
        }

        bool ttbr1_block(arch_phys_addr_t pa, int level, uint64_t attrs)
        {
            uint64_t const va = direct_va(pa);
            uint64_t* table = table_at(TTBR1_ROOT);
            if (level == 2)
            {
                table = ttbr1_child(table, index_at(va, 1));
                if (table == nullptr)
                {
                    return false;
                }
            }
            table[index_at(va, level)] = pa | 1u | DESC_AF | attrs;
            return true;
        }

        void ttbr1_record()
        {
            kickos_armv8a_direct_map_record();
        }

        uint32_t frames_freed_at_sweep()
        {
            return g_freed_at_sweep;
        }
    }
}
