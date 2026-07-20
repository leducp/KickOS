// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The instance-scoped kernel runtime core (invariant #7): several kernel
// instances co-reside in one host process (the KickCAT multi-slave sim -- one
// MCU == one instance). App-owned OBJECTS (TCBs, semaphores) stay caller-owned;
// this is only the runtime's own bookkeeping. The sim arch backend holds its own
// parallel SimInstance (arch/sim) and never crosses the arch seam (invariant #1).

#ifndef KICKOS_INSTANCE_H
#define KICKOS_INSTANCE_H

#include <stdint.h>
#include <stddef.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h>
#include <kickos/domain.h>
#include <kickos/endpoint.h>
#include <kickos/irq.h>
#include <kickos/list.h>
#include <kickos/slotpool.h>
#include <kickos/thread.h>
#include <kickos/sync.h>

namespace kickos
{
    struct SchedPolicy;

    struct Kernel
    {
        // --- scheduler mechanism (sched.cc) ---
        List ready[KICKOS_NUM_PRIO]; // one FIFO per priority; running thread at front
        uint32_t ready_bitmap = 0;   // bit p set iff ready[p] non-empty
        Thread* current = nullptr;
        Thread* idle = nullptr;
        unsigned live = 0; // non-idle threads not yet EXITED
        arch_context boot{};
        SchedPolicy const* policy = nullptr;

        // Per-Kernel monotonic thread-id counter (thread.cc). Starts at 0 so the
        // first thread created (idle, in kmain) is id 0; wraps skip 0 and 0xFFFF.
        uint16_t next_tid = 0;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        // --- telemetry counters (ktrace.h; instance-scoped, spike deliverable 5) ---
        // seq: monotonic per-record sequence (loss detection). records_attempted:
        // every record the frontend tried to emit (== last seq issued; carried in
        // SESSION for the host cross-check). dropped: records the sink refused
        // (ring full) -- records_attempted - dropped == records delivered.
        uint16_t trace_seq = 0;
        uint32_t trace_records_attempted = 0;
        uint32_t trace_dropped = 0;
        uint16_t trace_probe_overhead = 0; // measured once at ktrace_init (SESSION)
#endif

        // --- tickless time (time.cc) ---
        Thread* sleepq = nullptr; // sorted ascending by deadline_ns

        // --- syscall object pools (syscall.cc) ---
        // Semaphore registry: a generational slot pool (see slotpool.h). free() bumps
        // the slot's generation so a stale handle (index+gen) fails to resolve rather
        // than aliasing a recycled slot. All access goes through sem_resolve(). (gen
        // wraps every 2^16 destroys of one slot -- acceptable at this scale.)
        SlotPool<Semaphore, KICKOS_MAX_SEMAPHORES> sems;
        // Object-side refcount for the sem pool, owned by the cap layer (cap.cc):
        // "how many caps name slot i". alloc sets 1; delegate bumps; close decrements;
        // 0 => sems.free(). Parallel array so slotpool.h stays generic. uint8_t bounds
        // the max caps naming one object -- assert every table cannot exceed it.
        uint8_t sem_refs[KICKOS_MAX_SEMAPHORES] = {};
        static_assert(KICKOS_MAX_THREADS * KICKOS_MAX_HANDLES <= 255,
                      "sem_refs is uint8_t: MAX_THREADS x MAX_HANDLES must not exceed 255");
        // PI-mutex pool + its parallel object-side refcount, same shape as the sems
        // (cap.cc owns the accounting; the array keeps SlotPool generic).
        SlotPool<Mutex, KICKOS_MAX_MUTEXES> mutexes;
        uint8_t mutex_refs[KICKOS_MAX_MUTEXES] = {};
        static_assert(KICKOS_MAX_THREADS * KICKOS_MAX_HANDLES <= 255,
                      "mutex_refs is uint8_t: MAX_THREADS x MAX_HANDLES must not exceed 255");
        // Endpoint (IPC rendezvous) pool + its parallel object-side refcount, same
        // shape as sems/mutexes (cap.cc owns the accounting). recv_holders lives IN
        // the Endpoint struct, NOT here (its single home). endpoint_refs counts ALL
        // caps naming a slot; the wider bound covers it summing with the cap tables.
        SlotPool<Endpoint, KICKOS_MAX_ENDPOINTS> endpoints;
        uint8_t endpoint_refs[KICKOS_MAX_ENDPOINTS] = {};
        static_assert(KICKOS_MAX_THREADS * KICKOS_MAX_HANDLES + KICKOS_MAX_ENDPOINTS <= 255,
                      "endpoint_refs is uint8_t: MAX_THREADS x MAX_HANDLES + MAX_ENDPOINTS "
                      "must not exceed 255");
        // Thread pool (see ThreadPool in thread.h): the TCBs + their kernel stacks,
        // intrinsic liveness (a slot is free iff state==EXITED), generation bumped at
        // reclaim (ABA). All allocation goes through thread_spawn().
        ThreadPool threads;
        // Memory-domain pool (see domain.h): shared region sets threads reference.
        // domains[0] = kernel domain, domains[1] = default-user (both immortal);
        // the rest are refcounted mem_base domains. All access via domain_*().
        Domain domains[KICKOS_MAX_DOMAINS];

        // --- interrupt dispatch + IRQ-as-event bindings (irq.cc) ---
        IrqEntry irq_table[KICKOS_MAX_IRQ]; // line -> handler; ISR reads by index
        IrqBinding irq_bindings[KICKOS_MAX_IRQ_HANDLES];
        int irq_binding_count = 0;
        uint32_t irq_spurious_count = 0; // IRQs on a line with no driver (masked)
    };

    namespace detail
    {
        extern Kernel g_instance;
    }

    // The single access seam for instance-scoped state. Compile-time selectable
    // storage: a static singleton, or a per-host-thread instance for the
    // multi-slave sim (the thread-local pointer is installed per instance, Later).
    inline Kernel& kernel()
    {
#if defined(KICKOS_MULTI_INSTANCE)
        return *detail::g_instance_tls;
#else
        return detail::g_instance;
#endif
    }
}

#endif
