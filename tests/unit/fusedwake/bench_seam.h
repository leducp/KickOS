// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_TESTS_UNIT_FUSEDWAKE_BENCH_SEAM_H
#define KICKOS_TESTS_UNIT_FUSEDWAKE_BENCH_SEAM_H

#include <stdint.h>

namespace kickos::testfix
{
    // Switch count when PH_REPLY_RECV_TOTAL closed, plus the number of samples.
    // Ignore cycle deltas because host rdtsc timing is nondeterministic.
    // KSeam reset does not clear this state; tests must clear it themselves.
    extern uint32_t g_bench_seam_reply_recv_rows;
    extern uint32_t g_bench_seam_reply_recv_switches;
    void bench_seam_reset();
}

#endif
