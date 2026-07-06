// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Telemetry frontend out-of-line parts: the arch->kernel switch-completion hook,
// the SESSION bookends, and probe-overhead measurement. On ARM this TU is built
// -mgeneral-regs-only (see kernel/CMakeLists.txt): kickos_trace_switch_done runs
// in the PendSV tail with the incoming thread's FP state still live in registers,
// so the emit path must not touch any FP register.

#include <kickos/ktrace.h>

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>
#include <kickos/kernel.h>
#include <kickos/trace/record.h>

namespace kickos
{
    void ktrace_init(void)
    {
        IrqLock lock;
        Kernel& k = kernel();
        // Probe overhead: the cost of one arch_trace_now() read in trace ticks,
        // taken as the min of back-to-back deltas (a "null pair" cancels loop
        // bias). us-resolution backends read ~0; the DWT reads a few cycles. The
        // host subtracts this from measured switch/syscall latencies.
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
        // Opening SESSION (near clock anchor). ktrace_session takes its own lock;
        // IrqLock is nesting-safe, so holding one here is fine.
        ktrace_session();
    }
}

// Arch->kernel callback fired at the physical context swap (RESCAN group).
extern "C" void kickos_trace_switch_done(uint16_t from_tid, uint16_t to_tid)
{
    ::kickos::ktrace_switch(from_tid, to_tid);
}

// Closing SESSION (far clock anchor + final records_attempted); the sim flushes
// the ring right after this call.
extern "C" void kickos_trace_final_session(void)
{
    ::kickos::ktrace_session();
}

// One-line telemetry health report to the console (attempted vs dropped). Lets a
// CI gate cross-check the drop accounting against the decoded record count even
// when the closing SESSION itself was dropped by a full ring.
extern "C" void kickos_trace_report_counters(void)
{
    ::kickos::Kernel& k = ::kickos::kernel();
    ::kickos::kprintf("[ktrace] attempted=%u dropped=%u\n",
                      static_cast<unsigned>(k.trace_records_attempted),
                      static_cast<unsigned>(k.trace_dropped));
}

#endif
