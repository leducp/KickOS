// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What KICKOS_BENCH=1 leaves undefined once the K-seam sources and ipc_seam.cc answer the
// rest, re-derived with
//
//   nm --undefined-only <the objects> | comm -23 - <what those sources define>
//
// at two kernel cores: five symbols, kernel/bench/bench.cc being no part of this library.
// That file is not the answer, because the point of the substitution is the OBSERVATION
// below: bench_phase_add is an ordinary out-of-line call, so a seam standing in for it sees
// exactly WHEN a bracket closed relative to everything else the body does.
//
// The other four are answered so the link completes and are otherwise inert. g_bench_lock is
// written by the IrqLock bracket on every acquire in these sources, so it is a real array and
// not a stub.

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

    void bench_e2e_park_mark()
    {
    }
}
