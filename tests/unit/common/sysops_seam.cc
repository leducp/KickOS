// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See sysops_seam.h.

#include <kickos/arch/arch.h>

#include "sysops_seam.h"

namespace
{
    using kickos::testfix::SYSOPS_FRAMES;
    using kickos::testfix::SYSOPS_GRANULE;

    constexpr size_t OPS_CAP = 512;

    kickos::testfix::SysOp g_ops[OPS_CAP] = {};
    size_t g_ops_count = 0;
    // Treat trace overflow as a test failure.
    bool g_ops_overflow = false;

    uint32_t g_cpu = 0;

    size_t g_next_frame = 1;
    uint32_t g_frames_allocated = 0;
    uint32_t g_frames_freed = 0;
    uint32_t g_frame_budget = SYSOPS_FRAMES;

    void (*g_mid_edit)() = nullptr;
}

extern "C"
{
    // PA is the frame's byte offset into the backend seam's RAM array.
    arch_phys_addr_t kickos_frame_alloc(void)
    {
        if (g_next_frame >= SYSOPS_FRAMES or g_frame_budget == 0)
        {
            return 0;
        }
        g_frame_budget--;
        arch_phys_addr_t const frame =
            static_cast<arch_phys_addr_t>(g_next_frame * SYSOPS_GRANULE);
        g_next_frame++;
        g_frames_allocated++;
        return frame;
    }

    void kickos_frame_free(arch_phys_addr_t)
    {
        // Do not reuse frames; residency is keyed by root address.
        g_frames_freed++;
    }

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
}

namespace kickos
{
    namespace testfix
    {
        void sysop_record(char const* tag, uint64_t arg)
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

        void sysop_fire_mid_edit()
        {
            void (*fn)() = g_mid_edit;
            if (fn == nullptr)
            {
                return;
            }
            g_mid_edit = nullptr;
            fn();
        }

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

        void sysops_reset_common(size_t first_frame)
        {
            g_ops_count = 0;
            g_ops_overflow = false;
            g_cpu = 0;
            g_next_frame = first_frame;
            g_frames_allocated = 0;
            g_frames_freed = 0;
            g_frame_budget = SYSOPS_FRAMES;
            g_mid_edit = nullptr;
        }
    }
}
