// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the priority witness's three programs share (priority_low.cc, priority_high.cc and
// priority_pulse.cc; the composition names them).

#ifndef KICKOS_TESTS_INTEGRATION_COMPOSITION_WITNESS_PRIORITY_H
#define KICKOS_TESTS_INTEGRATION_COMPOSITION_WITNESS_PRIORITY_H

#include <kickos/sys.h>

#include <stdint.h>

namespace priority_witness
{
    constexpr uint64_t PULSE_NS = 200000000ull;
    // Past the pulse's first end, which falls within it.
    constexpr uint64_t HIGH_SPIN_NS = 600000000ull;
    // Past the restarted pulse's end.
    constexpr uint64_t LOW_SPIN_NS = 2000000000ull;

    // The pulse's deaths as the init last wrote them, or -1 where the record cannot be read.
    inline int pulse_deaths()
    {
        struct kos_task_status st;
        if (kos_task_status(0, &st) != 0)
        {
            return -1;
        }
        return st.deaths;
    }
}

#endif
