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
    constexpr size_t SEAM_GRANULE = 1u << KICKOS_RV64_GRANULE_SHIFT;
    constexpr size_t SEAM_FRAMES = 64;
    constexpr size_t OPS_CAP = 512;

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
    alignas(SEAM_GRANULE) unsigned char g_phys[SEAM_FRAMES * SEAM_GRANULE] = {};

    kickos::testfix::SysOp g_ops[OPS_CAP] = {};
    size_t g_ops_count = 0;
    // Treat trace overflow as a test failure.
    bool g_ops_overflow = false;

    uint32_t g_cpu = 0;
    uint64_t g_satp[KICKOS_NUM_CORES] = {};
    unsigned g_asid_bits = SEAM_ASID_BITS;

    size_t g_next_frame = FRAME_POOL_FIRST;
    uint32_t g_frames_allocated = 0;
    uint32_t g_frames_freed = 0;
    uint32_t g_frames_freed_at_rendezvous = 0;
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

    uint64_t* frame_ptr(size_t frame)
    {
        return reinterpret_cast<uint64_t*>(&g_phys[frame * SEAM_GRANULE]);
    }

    uint64_t boot_satp()
    {
        uint64_t const ppn = static_cast<uint64_t>(FRAME_BOOT_ROOT);
        return (static_cast<uint64_t>(KICKOS_RV64_SATP_MODE) << KICKOS_RV64_SATP_MODE_SHIFT) | ppn;
    }
}

extern "C"
{
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

    void kickos_rv64_translation_rendezvous(uint32_t peers)
    {
        g_frames_freed_at_rendezvous = g_frames_freed;
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

    uint64_t kickos_rv64_read_satp(void)
    {
        return g_satp[g_cpu];
    }

    void kickos_rv64_write_satp(uint64_t satp)
    {
        // Simulate WARL ASID bits by masking writes.
        uint64_t keep = 0;
        if (g_asid_bits != 0)
        {
            keep = ((1ull << g_asid_bits) - 1u) << SATP_ASID_SHIFT;
        }
        g_satp[g_cpu] = (satp & ~SATP_ASID_MASK) | (satp & keep);
        record(kickos::testfix::OP_WRITE_SATP, g_satp[g_cpu]);
    }

    void kickos_rv64_fence_w_w(void)
    {
        record(kickos::testfix::OP_FENCE_W_W, 0);
    }

    void kickos_rv64_sfence_page(uint64_t va)
    {
        record(kickos::testfix::OP_SFENCE_PAGE, va);
        fire_mid_edit();
    }

    void kickos_rv64_sfence_all(void)
    {
        record(kickos::testfix::OP_SFENCE_ALL, 0);
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

        uint64_t arg_of_nth(char const* tag, size_t n)
        {
            size_t seen = 0;
            for (size_t i = 0; i < g_ops_count; i++)
            {
                if (g_ops[i].tag != tag)
                {
                    continue;
                }
                if (seen == n)
                {
                    return g_ops[i].arg;
                }
                seen++;
            }
            return 0;
        }

        void set_cpu(uint32_t core)
        {
            if (core < KICKOS_NUM_CORES)
            {
                g_cpu = core;
            }
        }

        uint32_t cpu()
        {
            return g_cpu;
        }

        void arm_mid_edit(void (*fn)())
        {
            g_mid_edit = fn;
        }

        void sysops_reset(unsigned asid_bits)
        {
            g_ops_count = 0;
            g_ops_overflow = false;
            g_cpu = 0;
            g_asid_bits = asid_bits;
            if (g_asid_bits > SEAM_ASID_BITS)
            {
                g_asid_bits = SEAM_ASID_BITS;
            }
            g_next_frame = FRAME_POOL_FIRST;
            g_frames_allocated = 0;
            g_frames_freed = 0;
            g_frames_freed_at_rendezvous = 0;
            g_frame_budget = SEAM_FRAMES;
            g_mid_edit = nullptr;
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
                                    static_cast<arch_phys_addr_t>(SEAM_FRAMES * SEAM_GRANULE),
                                    SEAM_PHYS_BITS);
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

        uint64_t window_pa_hi()
        {
            return static_cast<uint64_t>(SEAM_FRAMES) * SEAM_GRANULE;
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
