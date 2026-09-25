// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the amp window and the forced fastpath leave undefined at an own-image node's posture.
// The doorbell only records: the arm is the peer, and plays its side by hand.

#include "amp_hold_seam.h"

#include <kickos/arch/arch.h>

namespace kickos
{
    namespace amphold
    {
        uint32_t g_raises = 0;
        uint32_t g_raise_mask = 0;
        uint32_t g_frame_results = 0;
        void (*g_after_snapshot)() = nullptr;
        uint32_t g_snapshot_hooks = 0;
    }

    void amp_hold_after_snapshot()
    {
        amphold::g_snapshot_hooks++;
        if (amphold::g_after_snapshot != nullptr)
        {
            amphold::g_after_snapshot();
        }
    }
}

extern "C"
{
    void arch_ipi_send(uint32_t cores)
    {
        kickos::amphold::g_raises++;
        kickos::amphold::g_raise_mask |= cores;
    }

    void arch_ipi_wait(uint32_t)
    {
    }

    // One thread of execution: the pairing this orders has no second side here.
    void arch_ipi_fence(void)
    {
    }

    void arch_ctx_set_syscall_result(struct arch_context*, uint32_t)
    {
        kickos::amphold::g_frame_results++;
    }
}
