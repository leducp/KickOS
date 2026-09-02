// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A per-core event ring for diagnosing a park that never woke. OFF unless KICKOS_SMP_TRACE is
// defined, and compiling to nothing when it is not: a diagnostic knob, never a posture.
//
// A thread that never runs again reads the same from the console in three cases, and the
// records separate them by ABSENCE:
//
//   no waker ran        a PARK with NO SEARCH of that queue after it.
//   a waker found none  a SEARCH with b = 0. The EMPTY record beside it carries the queue's
//                       head, which tells an empty queue from a search of the wrong one.
//   the switch never took   a SEARCH that hit, a READY, and no RUN.
//
// A REFUSED record is the fourth answer and is not a stall at all: the wake reached a thread
// whose state forbade it, and b carries that state.
//
// IT MAY NOT PERTURB WHAT IT MEASURES, which binds anyone editing the recording path: one ring
// per core on a line of its own, written only by that core, with no atomic, no lock and no
// barrier. The rings are read after the run out of guest memory.
//
// THE TIMESTAMP ORDERS RECORDS ACROSS CORES AND MUST STAY A READ. Every hart reads one timebase,
// so stamps compare without any core writing state another reads; a shared sequence counter
// would be exactly the cross-core write this instrument may not make.

#ifndef KICKOS_SMPTRACE_H
#define KICKOS_SMPTRACE_H

#include <stdint.h>

#if defined(KICKOS_SMP_TRACE) && KICKOS_SMP_TRACE

#include <kickos/arch/arch.h>
#include <kickos/instance.h>

namespace kickos
{
    // Records per core. A power of two, so the wrap is a mask and the write is branchless.
    constexpr uint32_t KOS_TRACE_DEPTH = 512;

    enum KosTraceKind : uint16_t
    {
        KOS_TR_PARK = 1,    // a: thread,        b: the queue it went on
        KOS_TR_SEARCH = 2,  // a: the queue,     b: the thread found, 0 for none
        KOS_TR_EMPTY = 3,   // a: the queue,     b: its head, 0 when genuinely empty
        KOS_TR_READY = 4,   // a: thread,        b: the priority it readied at
        KOS_TR_REFUSED = 5, // a: thread,        b: the state that refused the wake
        KOS_TR_ASK = 6,     // a: the peer mask, b: the priority that asked
        KOS_TR_RUN = 7,     // a: thread,        b: its priority
    };

    // Twenty-four bytes, fixed layout: the rings are decoded out of guest memory word by word,
    // so a field added here moves every offset the decoder reads. `seq` is this core's own
    // count and is what says whether the ring WRAPPED and lost the start of the story.
    struct KosTraceRec
    {
        uint32_t seq;
        uint16_t kind;
        uint16_t core;
        uint32_t a;
        uint32_t b;
        uint64_t ts;
    };

    struct alignas(64) KosTraceRing
    {
        uint32_t next; // this core's write cursor, monotonic and never reset
        uint32_t pad;
        KosTraceRec rec[KOS_TRACE_DEPTH];
    };

    // THE DECODER READS GUEST MEMORY WORD BY WORD, so these two sizes are its whole contract
    // with this header and the compiler is what holds them: tools/smptrace_decode.py carries
    // the same numbers, and a field added above moves every offset it reads.
    static_assert(sizeof(KosTraceRec) == 24, "tools/smptrace_decode.py unpacks 24-byte records");
    static_assert(sizeof(KosTraceRing) == 12352,
                  "tools/smptrace_decode.py strides rings by this many bytes");

    extern KosTraceRing g_kos_trace[KICKOS_KERNEL_CORES];

    // Every object here lives in the kernel half, so the low word identifies one uniquely
    // enough to correlate a park with the search that should have found it, and costs no
    // lookup on the recording path.
    inline uint32_t kos_trace_id(void const* p)
    {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    }

    inline void kos_trace(uint16_t kind, uint32_t a, uint32_t b)
    {
        uint32_t const me = kickos_kernel_core();
        if (me >= static_cast<uint32_t>(KICKOS_KERNEL_CORES))
        {
            return;
        }
        KosTraceRing& r = g_kos_trace[me];
        uint32_t const i = r.next & (KOS_TRACE_DEPTH - 1);
        // ONE WRITER PER RING AND NO READER WHILE THE IMAGE RUNS, so plain stores are the whole
        // discipline. An atomic here would add ordering the measured path does not have, which
        // is the instrument changing the interleaving it exists to observe.
        r.rec[i].seq = r.next;
        r.rec[i].kind = kind;
        r.rec[i].core = static_cast<uint16_t>(me);
        r.rec[i].a = a;
        r.rec[i].b = b;
        r.rec[i].ts = arch_clock_now();
        r.next = r.next + 1;
    }
}

#define KOS_TRACE(kind, a, b) ::kickos::kos_trace((kind), (a), (b))
#define KOS_TRACE_ID(p) ::kickos::kos_trace_id((p))

#else

#define KOS_TRACE(kind, a, b) ((void)0)
#define KOS_TRACE_ID(p) (0u)

#endif

#endif // KICKOS_SMPTRACE_H
