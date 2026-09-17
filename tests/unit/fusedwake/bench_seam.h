// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_TESTS_UNIT_FUSEDWAKE_BENCH_SEAM_H
#define KICKOS_TESTS_UNIT_FUSEDWAKE_BENCH_SEAM_H

#include <stdint.h>

namespace kickos::testfix
{
    // What the seam's bench_phase_add saw when PH_REPLY_RECV_TOTAL was recorded: the switch
    // count AT THAT INSTANT, which dates the close against the deferred wake's seat, and how
    // many times the row was taken, so a close that never happened cannot read as an ordering
    // that held.
    //
    // The DELTA the bracket passes is not kept: bench_cyccnt is rdtsc on this host.
    //
    // The state survives the KSeam fixture's reset(), which knows nothing about this seam, so
    // an arm that reads one clears it before it returns.
    extern uint32_t g_bench_seam_reply_recv_rows;
    extern uint32_t g_bench_seam_reply_recv_switches;
    void bench_seam_reset();
}

#endif
