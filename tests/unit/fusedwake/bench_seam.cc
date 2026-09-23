// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Record when PH_REPLY_RECV_TOTAL closes relative to scheduler switches.
// Stub other benchmark hooks and provide IrqLock timestamp storage.

#include <kickos/bench.h>

#include <kickos/sys/atomic.h>

#include "bench_seam.h"
#include "kfixture.h"

namespace kickos::testfix
{
    uint32_t g_bench_seam_reply_recv_rows = 0;
    uint32_t g_bench_seam_reply_recv_switches = 0;

    void bench_seam_reset()
    {
        g_bench_seam_reply_recv_rows = 0;
        g_bench_seam_reply_recv_switches = 0;
    }
}

namespace kickos
{
    constinit BenchLockRow g_bench_lock[KICKOS_KERNEL_CORES] = {};
    constinit Atomic<uint32_t, Order::RELAXED> g_bench_e2e_isr_core = BENCH_CORE_NONE;
    constinit Atomic<int32_t, Order::RELAXED> g_bench_e2e_line = -1;

    void bench_phase_add(uint32_t phase, BenchTick)
    {
        if (phase != PH_REPLY_RECV_TOTAL)
        {
            return;
        }
        testfix::g_bench_seam_reply_recv_rows++;
        testfix::g_bench_seam_reply_recv_switches = testfix::g_switches;
    }

    void bench_dist_add(uint32_t, BenchTick)
    {
    }

    void bench_lock_hold_add(BenchTick, void*)
    {
    }

    void bench_e2e_park_mark()
    {
    }
}
