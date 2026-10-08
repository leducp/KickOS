// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Telemetry frontend over the pure record encoders and the RTT ch1 sink. Assigning the
// sequence number, sampling arch_trace_now() and writing the record are ONE atomic region
// under a single IrqLock: split them and a preempting context issues an out-of-order
// seq/stamp that silently corrupts the host's loss and latency accounting.
//
// The counters are instance-scoped (Kernel), so the multi-instance sim never shares them.

#ifndef KICKOS_KTRACE_H
#define KICKOS_KTRACE_H

#include <stdint.h>
#include <stddef.h>

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/rtt.h>
#include <kickos/trace/record.h>

// Architecture id baked in by the build.
#ifndef KICKOS_TRACE_ARCH
#define KICKOS_TRACE_ARCH 0
#endif

namespace kickos
{
    namespace ktrace_detail
    {
        // MUST be called with the emit IrqLock held (records_attempted/dropped are RMW).
        inline void put(Kernel& k, uint8_t const* rec, size_t n)
        {
            k.trace_records_attempted++;
            if (kickos_rtt_write_record_ch1(rec, n) == 0)
            {
                k.trace_dropped++;
            }
        }
    }

    inline void ktrace_switch(uint16_t from_tid, uint16_t to_tid)
    {
        IrqLock lock;
        Kernel& k = kernel();
        uint16_t seq = k.trace_seq++;
        uint32_t t = arch_trace_now();
        uint8_t rec[trace::TRACE_MAX_RECORD];
        size_t n = trace::encode_switch(rec, seq, t, from_tid, to_tid);
        ktrace_detail::put(k, rec, n);
    }

    inline void ktrace_syscall_enter(uint16_t tid, uint16_t nr)
    {
        IrqLock lock;
        Kernel& k = kernel();
        uint16_t seq = k.trace_seq++;
        uint32_t t = arch_trace_now();
        uint8_t rec[trace::TRACE_MAX_RECORD];
        size_t n = trace::encode_syscall_enter(rec, seq, t, tid, nr);
        ktrace_detail::put(k, rec, n);
    }

    inline void ktrace_syscall_exit(uint16_t tid, uint16_t nr)
    {
        IrqLock lock;
        Kernel& k = kernel();
        uint16_t seq = k.trace_seq++;
        uint32_t t = arch_trace_now();
        uint8_t rec[trace::TRACE_MAX_RECORD];
        size_t n = trace::encode_syscall_exit(rec, seq, t, tid, nr);
        ktrace_detail::put(k, rec, n);
    }

    inline void ktrace_irq_enter(uint16_t line)
    {
        IrqLock lock;
        Kernel& k = kernel();
        uint16_t seq = k.trace_seq++;
        uint32_t t = arch_trace_now();
        uint8_t rec[trace::TRACE_MAX_RECORD];
        size_t n = trace::encode_irq_enter(rec, seq, t, line);
        ktrace_detail::put(k, rec, n);
    }

    inline void ktrace_irq_exit(uint16_t line)
    {
        IrqLock lock;
        Kernel& k = kernel();
        uint16_t seq = k.trace_seq++;
        uint32_t t = arch_trace_now();
        uint8_t rec[trace::TRACE_MAX_RECORD];
        size_t n = trace::encode_irq_exit(rec, seq, t, line);
        ktrace_detail::put(k, rec, n);
    }

    // (t, t_anchor) is one point of the two-anchor clock resync; records_attempted lets the
    // host cross-check its lost count. Emitted twice per session.
    inline void ktrace_session(void)
    {
        IrqLock lock;
        Kernel& k = kernel();
        uint16_t seq = k.trace_seq++;
        k.trace_records_attempted++; // counts the SESSION itself
        uint32_t t = arch_trace_now();
        uint64_t t_anchor = arch_clock_now();
        uint8_t rec[trace::TRACE_MAX_RECORD];
        size_t n = trace::encode_session(rec, seq, t,
                                         static_cast<uint8_t>(KICKOS_TRACE_ARCH), 32,
                                         k.trace_probe_overhead,
                                         k.trace_records_attempted, t_anchor);
        if (kickos_rtt_write_record_ch1(rec, n) == 0)
        {
            k.trace_dropped++;
        }
    }

    // Measure the probe overhead and emit the opening SESSION. Once per boot, after the arch
    // clock is live.
    void ktrace_init(void);
}

#else

namespace kickos
{
    inline void ktrace_switch(uint16_t, uint16_t) {}
    inline void ktrace_syscall_enter(uint16_t, uint16_t) {}
    inline void ktrace_syscall_exit(uint16_t, uint16_t) {}
    inline void ktrace_irq_enter(uint16_t) {}
    inline void ktrace_irq_exit(uint16_t) {}
    inline void ktrace_init(void) {}
}

#endif

#endif
