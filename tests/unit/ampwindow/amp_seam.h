// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The observable half of the seam kernel/amp/ampwindow.cc is compiled against here. `g_node`
// is what arch_cpu_id answers, so one arm can be the producer and then the consumer of a ring.

#ifndef KICKOS_TESTS_UNIT_AMPWINDOW_AMP_SEAM_H
#define KICKOS_TESTS_UNIT_AMPWINDOW_AMP_SEAM_H

#include <stdint.h>

namespace kickos
{
    namespace ampfix
    {
        // Which node arch_cpu_id answers. An arm sets it around the call it wants attributed.
        extern uint32_t g_node;

        // Doorbells the window raised, and the union of the core masks they named.
        extern uint32_t g_sends;
        extern uint32_t g_sent_mask;

        void reset();
    }
}

#endif
