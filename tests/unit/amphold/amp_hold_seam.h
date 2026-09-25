// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_TESTS_UNIT_AMPHOLD_AMP_HOLD_SEAM_H
#define KICKOS_TESTS_UNIT_AMPHOLD_AMP_HOLD_SEAM_H

#include <stdint.h>

namespace kickos
{
    namespace amphold
    {
        // Doorbell raises and the union of the core masks they named.
        extern uint32_t g_raises;
        extern uint32_t g_raise_mask;
        // Results the switch stored into a fastpath caller's saved frame.
        extern uint32_t g_frame_results;
        // Fired after far_hold_copy snapshots a slot, before its second liveness check.
        extern void (*g_after_snapshot)();
        extern uint32_t g_snapshot_hooks;
    }
}

#endif
