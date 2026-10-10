// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See x86_64_sysops_seam.h.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>
#include <kickos/chip_com1.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x86_64_sysops_seam.h"

namespace
{
    constexpr size_t GRANULE = 4096;
    constexpr size_t PTES = 512;
    constexpr uint64_t PTE_P = 1ull << 0;
    constexpr uint64_t PTE_RW = 1ull << 1;
    constexpr uint64_t PTE_US = 1ull << 2;
    constexpr uint64_t PTE_PS = 1ull << 7;
    constexpr unsigned SLOT_SHIFT = 39;
    constexpr unsigned GIB_SHIFT = 30;
    constexpr unsigned MIB2_SHIFT = 21;
    constexpr uint64_t PTE_ADDR = 0x000ffffffffff000ull;

    // Wide enough for any host address the pool and the backend's tables sit at.
    constexpr uint32_t MAXPHYADDR = 48;
    constexpr uint32_t CPUID_EDX_PAT = 1u << 16;
    constexpr uint32_t CPUID_EDX_NX = 1u << 20;

    constexpr uint64_t CR0_PE = 1ull << 0;
    constexpr uint64_t CR0_WP = 1ull << 16;
    constexpr uint64_t CR0_PG = 1ull << 31;
    constexpr uint64_t CR4_PAE = 1ull << 5;
    constexpr uint64_t CR4_LA57 = 1ull << 12;
    constexpr uint64_t EFER_LME_LMA = 0x500;
    constexpr uint64_t PAT_POWER_UP = 0x0007040600070406ull;
    constexpr uint32_t MSR_EFER = 0xc0000080;
    constexpr uint32_t MSR_PAT = 0x277;

    // The firmware's root and a level-3 table for each slot it identity-maps.
    alignas(GRANULE) uint64_t g_firmware[4][PTES] = {};
    // A level-2 and a level-1 table splitting the RAM array's leaf, and a five-level root.
    alignas(GRANULE) uint64_t g_deeper[3][PTES] = {};

    alignas(GRANULE) unsigned char g_ram[kickos::testfix::SYSOPS_FRAMES * GRANULE] = {};

    uint64_t g_cr0 = 0;
    uint64_t g_cr3[KICKOS_NUM_CORES] = {};
    uint64_t g_cr4 = 0;
    uint64_t g_efer = 0;
    uint64_t g_pat = 0;

    struct arch_aspace* g_watch = nullptr;
    uint32_t g_freed_at_rendezvous = 0;

    uint64_t* model_register(uint32_t index)
    {
        if (index == MSR_EFER)
        {
            return &g_efer;
        }
        if (index == MSR_PAT)
        {
            return &g_pat;
        }
        abort();
    }

    // Maps the whole top-level slot holding `addr` onto itself with 1 GiB leaves.
    void identity_slot(uintptr_t addr, uint64_t* level3)
    {
        size_t const slot = (addr >> SLOT_SHIFT) & (PTES - 1u);
        if ((g_firmware[0][slot] & PTE_P) != 0)
        {
            return;
        }
        uintptr_t const slot_base = addr & ~((static_cast<uintptr_t>(1) << SLOT_SHIFT) - 1u);
        for (size_t i = 0; i < PTES; i++)
        {
            level3[i] = (slot_base + (static_cast<uintptr_t>(i) << GIB_SHIFT)) | PTE_P | PTE_RW
                        | PTE_PS;
        }
        g_firmware[0][slot] = reinterpret_cast<uintptr_t>(level3) | PTE_P | PTE_RW;
    }
}

extern "C"
{
    // One byte both symbols name, so the app window is empty.
    unsigned char kickos_seam_app_window[1] = {};
    extern unsigned char __kickos_app_rom_start[1] __attribute__((alias("kickos_seam_app_window")));
    extern unsigned char __kickos_app_sram_end[1] __attribute__((alias("kickos_seam_app_window")));

    void kickos_x86_64_cpuid(uint32_t leaf, uint32_t, uint32_t* a, uint32_t* b, uint32_t* c,
                             uint32_t* d)
    {
        *a = 0;
        *b = 0;
        *c = 0;
        *d = 0;
        if (leaf == 0)
        {
            *a = 7;
        }
        else if (leaf == 1)
        {
            *d = CPUID_EDX_PAT;
        }
        else if (leaf == 0x80000000u)
        {
            *a = 0x80000008u;
        }
        else if (leaf == 0x80000001u)
        {
            *d = CPUID_EDX_NX;
        }
        else if (leaf == 0x80000008u)
        {
            *a = MAXPHYADDR;
        }
    }

    void kickos_x86_64_invlpg(uintptr_t va)
    {
        kickos::testfix::sysop_record(kickos::testfix::OP_INVLPG, va);
        kickos::testfix::sysop_record(kickos::testfix::OP_INVLPG_LEAF,
                                      kickos::x86_64::aspace_leaf_desc(g_watch, va));
    }

    uint64_t kickos_x86_64_read_cr0(void)
    {
        return g_cr0;
    }

    void kickos_x86_64_write_cr0(uint64_t v)
    {
        g_cr0 = v;
    }

    uint64_t kickos_x86_64_read_cr3(void)
    {
        return g_cr3[kickos::testfix::cpu()];
    }

    void kickos_x86_64_write_cr3(uint64_t v)
    {
        g_cr3[kickos::testfix::cpu()] = v;
    }

    uint64_t kickos_x86_64_read_cr4(void)
    {
        return g_cr4;
    }

    uint64_t kickos_x86_64_read_msr(uint32_t index)
    {
        return *model_register(index);
    }

    void kickos_x86_64_write_msr(uint32_t index, uint64_t value)
    {
        *model_register(index) = value;
    }

    void kfault_terminate(void)
    {
        abort();
    }

    uint32_t kickos_x86_64_online_cores(void)
    {
        return (1u << KICKOS_NUM_CORES) - 1u;
    }

    void kickos_x86_64_translation_rendezvous(uint32_t peers)
    {
        g_freed_at_rendezvous = kickos::testfix::frames_freed();
        kickos::testfix::sysop_record(kickos::testfix::OP_RENDEZVOUS, peers);
    }
}

namespace kickos
{
    namespace q35
    {
        void com1_puts(char const* s)
        {
            fputs(s, stderr);
        }
    }

    namespace testfix
    {
        char const* const OP_INVLPG = "invlpg";
        char const* const OP_INVLPG_LEAF = "invlpg leaf";
        char const* const OP_RENDEZVOUS = "translation_rendezvous";

        void sysops_reset()
        {
            memset(g_ram, 0, sizeof(g_ram));
            sysops_reset_common(1, reinterpret_cast<uintptr_t>(g_ram));
            g_watch = nullptr;
            g_freed_at_rendezvous = 0;

            memset(g_firmware, 0, sizeof(g_firmware));
            memset(g_deeper, 0, sizeof(g_deeper));
            identity_slot(reinterpret_cast<uintptr_t>(g_ram), g_firmware[1]);
            // The backend's own tables live in this image beside the seam's.
            identity_slot(reinterpret_cast<uintptr_t>(&g_firmware[0][0]), g_firmware[2]);
            // Slot 0, where device and unpooled outputs sit: acquire reaches a frame by identity.
            identity_slot(0, g_firmware[3]);

            g_cr0 = CR0_PE | CR0_WP | CR0_PG;
            g_cr4 = CR4_PAE;
            g_efer = EFER_LME_LMA;
            g_pat = PAT_POWER_UP;
            for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
            {
                g_cr3[c] = reinterpret_cast<uintptr_t>(&g_firmware[0][0]);
            }
            // No arena: its alias above the user offset needs this image below the user slots,
            // which a host does not promise.
            kickos::x86_64::aspace_init(0, 0);
            ops_clear();
        }

        void watch_space(struct arch_aspace* space)
        {
            g_watch = space;
        }

        uint32_t frames_freed_at_rendezvous()
        {
            return g_freed_at_rendezvous;
        }

        void firmware_user_chain(int leaf_level, int clear_level, bool five_levels)
        {
            uintptr_t const at = reinterpret_cast<uintptr_t>(g_ram);
            uint64_t* const l3 = g_firmware[1];
            if (leaf_level <= 2)
            {
                uintptr_t const base = at & ~((static_cast<uintptr_t>(1) << GIB_SHIFT) - 1u);
                for (size_t i = 0; i < PTES; i++)
                {
                    g_deeper[0][i] = (base + (static_cast<uintptr_t>(i) << MIB2_SHIFT)) | PTE_P
                                     | PTE_RW | PTE_PS;
                }
                l3[(at >> GIB_SHIFT) & (PTES - 1u)] =
                    reinterpret_cast<uintptr_t>(g_deeper[0]) | PTE_P | PTE_RW;
            }
            if (leaf_level == 1)
            {
                uintptr_t const base = at & ~((static_cast<uintptr_t>(1) << MIB2_SHIFT) - 1u);
                for (size_t i = 0; i < PTES; i++)
                {
                    g_deeper[1][i] = (base + i * GRANULE) | PTE_P | PTE_RW;
                }
                g_deeper[0][(at >> MIB2_SHIFT) & (PTES - 1u)] =
                    reinterpret_cast<uintptr_t>(g_deeper[1]) | PTE_P | PTE_RW;
            }
            int top = 4;
            uint64_t* table = g_firmware[0];
            if (five_levels)
            {
                top = 5;
                table = g_deeper[2];
                table[(at >> (SLOT_SHIFT + 9)) & (PTES - 1u)] =
                    reinterpret_cast<uintptr_t>(g_firmware[0]) | PTE_P | PTE_RW;
                g_cr4 |= CR4_LA57;
                for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
                {
                    g_cr3[c] = reinterpret_cast<uintptr_t>(table);
                }
            }
            for (int level = top; level >= leaf_level; level--)
            {
                unsigned const shift = 12u + 9u * static_cast<unsigned>(level - 1);
                uint64_t* const entry = &table[(at >> shift) & (PTES - 1u)];
                if (level != clear_level)
                {
                    *entry |= PTE_US;
                }
                table = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(*entry & PTE_ADDR));
            }
        }

        void adopt()
        {
            kickos::x86_64::aspace_init(0, 0);
        }
    }
}
