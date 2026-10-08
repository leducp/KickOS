// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// On ARM this TU is built -mgeneral-regs-only: kickos_trace_switch_done runs in the PendSV
// tail with the incoming thread's FP state still live, so the emit path must not touch any
// FP register.

#include <kickos/ktrace.h>

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>
#include <kickos/kernel.h>
#include <kickos/trace/record.h>

namespace kickos
{
    // record.h is a lower, dependency-free layer and cannot see thread.h, so the
    // "no thread" sentinel is spelled twice; keep them in lockstep here.
    static_assert(trace::TRACE_NO_THREAD == KICKOS_TID_NONE,
                  "trace::TRACE_NO_THREAD must equal KICKOS_TID_NONE");

    void ktrace_init(void)
    {
        IrqLock lock;
        Kernel& k = kernel();
        // The min of two back-to-back arch_trace_now() deltas; the host subtracts it from
        // measured latencies.
        uint32_t t0 = arch_trace_now();
        uint32_t t1 = arch_trace_now();
        uint32_t t2 = arch_trace_now();
        uint32_t d1 = t1 - t0;
        uint32_t d2 = t2 - t1;
        uint32_t d = d1;
        if (d2 < d)
        {
            d = d2;
        }
        if (d > 0xFFFFu)
        {
            d = 0xFFFFu;
        }
        k.trace_probe_overhead = static_cast<uint16_t>(d);
        // ktrace_session takes its own lock; IrqLock nests.
        ktrace_session();
    }
}

extern "C" void kickos_trace_switch_done(uint16_t from_tid, uint16_t to_tid)
{
    ::kickos::ktrace_switch(from_tid, to_tid);
}

extern "C" void kickos_trace_final_session(void)
{
    ::kickos::ktrace_session();
}

// Lets a CI gate cross-check the drop accounting even when the closing SESSION was dropped.
extern "C" void kickos_trace_report_counters(void)
{
    ::kickos::Kernel& k = ::kickos::kernel();
    ::kickos::kprintf("[ktrace] attempted=%u dropped=%u\n",
                      static_cast<unsigned>(k.trace_records_attempted),
                      static_cast<unsigned>(k.trace_dropped));
}

#endif
