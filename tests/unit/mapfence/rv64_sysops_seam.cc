// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See rv64_sysops_seam.h.

#include <kickos/arch/arch.h>
#include <kickos/arch/rv64_paging.h>

#include "rv64_sysops_seam.h"

extern "C" void kickos_rv64_aspace_boot(uint64_t* user_root, uint64_t* window_leaves,
                                        uintptr_t window_va, uintptr_t window_delta,
                                        arch_phys_addr_t pa_lo, arch_phys_addr_t pa_hi,
                                        unsigned phys_bits);

namespace
{
    using kickos::testfix::cpu;
    using kickos::testfix::SYSOPS_FRAMES;
    using kickos::testfix::SYSOPS_GRANULE;
    using kickos::testfix::sysop_fire_mid_edit;
    using kickos::testfix::sysop_record;

    static_assert((size_t{1} << KICKOS_RV64_GRANULE_SHIFT) == SYSOPS_GRANULE,
                  "the shared frame pool hands out frames of the Sv39 granule");

    // Reserve frame 0 for allocation failure, frame 1 for the boot root,
    // and frame 2 for the transient-window leaf table.
    constexpr size_t FRAME_BOOT_ROOT = 1;
    constexpr size_t FRAME_WINDOW_LEAVES = 2;
    constexpr size_t FRAME_POOL_FIRST = 3;

    constexpr uintptr_t SEAM_WINDOW_VA = 0xFFFFFFD000000000ull;
    constexpr unsigned SEAM_PHYS_BITS = 40;

    constexpr unsigned SATP_ASID_SHIFT = 44;
    constexpr uint64_t SATP_ASID_MASK = 0xFFFFull << SATP_ASID_SHIFT;
    constexpr unsigned SEAM_ASID_BITS = 16;

    // The array models the RAM window; physical addresses are byte offsets into it.
    alignas(SYSOPS_GRANULE) unsigned char g_phys[SYSOPS_FRAMES * SYSOPS_GRANULE] = {};

    uint64_t g_satp[KICKOS_NUM_CORES] = {};
    unsigned g_asid_bits = SEAM_ASID_BITS;

    uint32_t g_frames_freed_at_rendezvous = 0;

    uint64_t* frame_ptr(size_t frame)
    {
        return reinterpret_cast<uint64_t*>(&g_phys[frame * SYSOPS_GRANULE]);
    }

    uint64_t boot_satp()
    {
        uint64_t const ppn = static_cast<uint64_t>(FRAME_BOOT_ROOT);
        return (static_cast<uint64_t>(KICKOS_RV64_SATP_MODE) << KICKOS_RV64_SATP_MODE_SHIFT) | ppn;
    }
}

extern "C"
{
    void kickos_rv64_translation_rendezvous(uint32_t peers)
    {
        g_frames_freed_at_rendezvous = kickos::testfix::frames_freed();
        sysop_record(kickos::testfix::OP_RENDEZVOUS, peers);
    }

    uint64_t kickos_rv64_read_satp(void)
    {
        return g_satp[cpu()];
    }

    void kickos_rv64_write_satp(uint64_t satp)
    {
        // Simulate WARL ASID bits by masking writes.
        uint64_t keep = 0;
        if (g_asid_bits != 0)
        {
            keep = ((1ull << g_asid_bits) - 1u) << SATP_ASID_SHIFT;
        }
        g_satp[cpu()] = (satp & ~SATP_ASID_MASK) | (satp & keep);
        sysop_record(kickos::testfix::OP_WRITE_SATP, g_satp[cpu()]);
    }

    void kickos_rv64_fence_w_w(void)
    {
        sysop_record(kickos::testfix::OP_FENCE_W_W, 0);
    }

    void kickos_rv64_sfence_page(uint64_t va)
    {
        sysop_record(kickos::testfix::OP_SFENCE_PAGE, va);
        sysop_fire_mid_edit();
    }

    void kickos_rv64_sfence_all(void)
    {
        sysop_record(kickos::testfix::OP_SFENCE_ALL, 0);
    }
}

namespace kickos
{
    namespace testfix
    {
        char const* const OP_FENCE_W_W = "fence w, w";
        char const* const OP_SFENCE_PAGE = "sfence.vma va, zero";
        char const* const OP_SFENCE_ALL = "sfence.vma zero, zero";
        char const* const OP_WRITE_SATP = "csrw satp";
        char const* const OP_RENDEZVOUS = "translation_rendezvous";

        void sysops_reset(unsigned asid_bits)
        {
            sysops_reset_common(FRAME_POOL_FIRST);
            g_asid_bits = asid_bits;
            if (g_asid_bits > SEAM_ASID_BITS)
            {
                g_asid_bits = SEAM_ASID_BITS;
            }
            g_frames_freed_at_rendezvous = 0;
            for (size_t i = 0; i < sizeof(g_phys); i++)
            {
                g_phys[i] = 0;
            }
            // Initialize every hart with the boot root before handover.
            for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
            {
                g_satp[c] = boot_satp();
            }
            kickos_rv64_aspace_boot(frame_ptr(FRAME_BOOT_ROOT), frame_ptr(FRAME_WINDOW_LEAVES),
                                    SEAM_WINDOW_VA, reinterpret_cast<uintptr_t>(g_phys), 0,
                                    static_cast<arch_phys_addr_t>(SYSOPS_FRAMES * SYSOPS_GRANULE),
                                    SEAM_PHYS_BITS);
        }

        uint64_t window_pa_hi()
        {
            return static_cast<uint64_t>(SYSOPS_FRAMES) * SYSOPS_GRANULE;
        }

        uintptr_t window_va()
        {
            return SEAM_WINDOW_VA;
        }

        uintptr_t window_delta()
        {
            return reinterpret_cast<uintptr_t>(g_phys);
        }

        uint32_t frames_freed_at_rendezvous()
        {
            return g_frames_freed_at_rendezvous;
        }
    }
}
