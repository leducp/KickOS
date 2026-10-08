// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_TESTS_UNIT_FUSEDWAKE_BENCH_SEAM_H
#define KICKOS_TESTS_UNIT_FUSEDWAKE_BENCH_SEAM_H

#include <stdint.h>

#include <kickos/thread.h>

namespace kickos::testfix
{
    // The last bench_e2e_park_mark call: whether the kernel lock was held, which thread ran it
    // and that thread's state, and the parks committed before it.
    struct BenchParkMark
    {
        uint32_t calls;
        bool locked;
        Thread* by;
        ThreadState state;
        uint32_t parks;
    };

    // Switch count when PH_REPLY_RECV_TOTAL closed, plus the number of samples.
    // Ignore cycle deltas because host rdtsc timing is nondeterministic.
    // KSeam reset does not clear this state; tests must clear it themselves.
    extern uint32_t g_bench_seam_reply_recv_rows;
    extern uint32_t g_bench_seam_reply_recv_switches;
    extern BenchParkMark g_bench_seam_park_mark;
    void bench_seam_reset();
}

#endif
