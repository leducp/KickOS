// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See aspace_sysops_seam.h.

#include <kickos/arch/arch.h>

#include "aspace_sysops_seam.h"

namespace
{
    constexpr size_t SEAM_GRANULE = 4096;
    constexpr size_t SEAM_FRAMES = 64;
    constexpr size_t OPS_CAP = 512;

    kickos::testfix::SysOp g_ops[OPS_CAP] = {};
    size_t g_ops_count = 0;
    // Treat trace overflow as a test failure.
    bool g_ops_overflow = false;

    uint32_t g_cpu = 0;
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

    size_t g_next_frame = 1; // frame 0 is never handed out: 0 is the pool's failure answer
    uint32_t g_frames_allocated = 0;
    uint32_t g_frames_freed = 0;
    uint32_t g_frame_budget = SEAM_FRAMES;

    void (*g_mid_edit)() = nullptr;

    void record(char const* tag, uint64_t arg)
    {
        if (g_ops_count >= OPS_CAP)
        {
            g_ops_overflow = true;
            return;
        }
        g_ops[g_ops_count].tag = tag;
        g_ops[g_ops_count].arg = arg;
        g_ops_count++;
    }

    void fire_mid_edit()
    {
        void (*fn)() = g_mid_edit;
        if (fn == nullptr)
        {
            return;
        }
        g_mid_edit = nullptr;
        fn();
    }
}

extern "C"
{
    // The array models the physical RAM window; PA is its byte offset.
    // Frame zero is reserved.
    alignas(SEAM_GRANULE) unsigned char __kickos_arm64_va_base[SEAM_FRAMES * SEAM_GRANULE] = {};

    arch_phys_addr_t kickos_frame_alloc(void)
    {
        if (g_next_frame >= SEAM_FRAMES or g_frame_budget == 0)
        {
            return 0;
        }
        g_frame_budget--;
        arch_phys_addr_t const frame =
            static_cast<arch_phys_addr_t>(g_next_frame * SEAM_GRANULE);
        g_next_frame++;
        g_frames_allocated++;
        return frame;
    }

    void kickos_frame_free(arch_phys_addr_t)
    {
        // Do not reuse frames; residency is keyed by root address.
        g_frames_freed++;
    }

    void kickos_arm64_instruction_side_rendezvous(uint32_t peers)
    {
        record(kickos::testfix::OP_RENDEZVOUS, peers);
    }

// Keep this guard spelling for check_cpu_id_fold.sh.
#if KICKOS_NUM_CORES > 1
    uint32_t arch_cpu_id(void)
    {
        return g_cpu;
    }
#endif

    arch_irq_state_t arch_irq_save(void)
    {
        return 0;
    }

    void arch_irq_restore(arch_irq_state_t)
    {
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
        return g_ttbr0[g_cpu];
    }

    void kickos_armv8a_write_ttbr0_el1(uint64_t ttbr)
    {
        g_ttbr0[g_cpu] = ttbr;
        record(kickos::testfix::OP_WRITE_TTBR0, ttbr);
    }

    void kickos_armv8a_dsb_ishst(void)
    {
        record(kickos::testfix::OP_DSB_ISHST, 0);
    }

    void kickos_armv8a_dsb_ish(void)
    {
        record(kickos::testfix::OP_DSB_ISH, 0);
    }

    void kickos_armv8a_isb(void)
    {
        record(kickos::testfix::OP_ISB, 0);
    }

    void kickos_armv8a_tlbi_page_is(uint64_t page)
    {
        record(kickos::testfix::OP_TLBI_PAGE_IS, page);
        fire_mid_edit();
    }

    void kickos_armv8a_tlbi_page_local(uint64_t page)
    {
        record(kickos::testfix::OP_TLBI_PAGE_LOCAL, page);
        fire_mid_edit();
    }

    void kickos_armv8a_tlbi_all_is(void)
    {
        record(kickos::testfix::OP_TLBI_ALL_IS, 0);
    }

    void kickos_armv8a_tlbi_all_local(void)
    {
        record(kickos::testfix::OP_TLBI_ALL_LOCAL, 0);
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

        size_t ops_count()
        {
            if (g_ops_overflow)
            {
                return OPS_CAP + 1; // reads as neither an order nor a count, and no arm's figure
            }
            return g_ops_count;
        }

        void ops_clear()
        {
            g_ops_count = 0;
            g_ops_overflow = false;
        }

        SysOp op_at(size_t i)
        {
            if (i >= g_ops_count)
            {
                SysOp const none = {nullptr, 0};
                return none;
            }
            return g_ops[i];
        }

        size_t ops_with(char const* tag)
        {
            size_t n = 0;
            for (size_t i = 0; i < g_ops_count; i++)
            {
                if (g_ops[i].tag == tag)
                {
                    n++;
                }
            }
            return n;
        }

        int last_index_of(char const* tag)
        {
            int found = -1;
            for (size_t i = 0; i < g_ops_count; i++)
            {
                if (g_ops[i].tag == tag)
                {
                    found = static_cast<int>(i);
                }
            }
            return found;
        }

        void set_cpu(uint32_t core)
        {
            if (core < KICKOS_NUM_CORES)
            {
                g_cpu = core;
            }
        }

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

        uint32_t cpu()
        {
            return g_cpu;
        }

        void arm_mid_edit(void (*fn)())
        {
            g_mid_edit = fn;
        }

        void sysops_reset()
        {
            g_ops_count = 0;
            g_ops_overflow = false;
            g_cpu = 0;
            for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
            {
                g_ttbr0[c] = 0;
            }
            g_next_frame = 1;
            g_frames_allocated = 0;
            g_frames_freed = 0;
            g_frame_budget = SEAM_FRAMES;
            g_mid_edit = nullptr;
            g_tcr = TCR_DEFAULT;
            g_mmfr0 = MMFR0_DEFAULT;
        }

        uint32_t frames_allocated()
        {
            return g_frames_allocated;
        }

        uint32_t frames_freed()
        {
            return g_frames_freed;
        }

        void set_frame_budget(uint32_t frames)
        {
            g_frame_budget = frames;
        }
    }
}
