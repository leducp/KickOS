// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The instance-scoped kernel runtime core. Several kernel instances may co-reside in one
// host process, one per simulated MCU, so nothing here may become a file-static. App-owned
// OBJECTS (TCBs, semaphores) stay caller-owned; this is only the runtime's own bookkeeping.
// The sim arch backend keeps its own parallel SimInstance and never crosses the arch seam.
//
// State a module owns PRIVATELY stays where it is and wraps in InstanceLocal
// (instance_local.h) rather than moving in here.

#ifndef KICKOS_INSTANCE_H
#define KICKOS_INSTANCE_H

#include <stdint.h>
#include <stddef.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h>
#include <kickos/domain.h>
#include <kickos/endpoint.h>
#include <kickos/instance_local.h>
#include <kickos/irq.h>
#include <kickos/list.h>
#include <kickos/slotpool.h>
#include <kickos/task.h>
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
        // --- telemetry counters (ktrace.h) ---
        // trace_records_attempted equals the last seq issued and is carried in SESSION for
        // the host cross-check; attempted minus dropped is what was delivered.
        uint16_t trace_seq = 0;
        uint32_t trace_records_attempted = 0;
        uint32_t trace_dropped = 0;
        uint16_t trace_probe_overhead = 0; // measured once at ktrace_init (SESSION)
#endif

        // Tasks currently holding a creator hold (task.cc). Declared HERE, away from the
        // task pool below, because the two bytes before `sleepq` are padding on every
        // 32-bit target in BOTH telemetry postures: microbit's `_ebss` is its arena base,
        // so a byte that grows the struct costs a whole allocation granule.
        uint16_t task_holds = 0;

        // --- tickless time (time.cc) ---
        Thread* sleepq = nullptr; // sorted ascending by deadline_ns

        // --- syscall object pools (syscall.cc) ---
        // Semaphore registry: a generational slot pool (see slotpool.h). Reached only
        // through its own resolve(); the generation wraps every 2^16 destroys of one slot.
        SlotPool<Semaphore, KICKOS_MAX_SEMAPHORES> sems;
        // Object-side refcount owned by the cap layer: how many caps name slot i. alloc
        // sets 1, delegate bumps, close decrements, and 0 frees the slot. The uint8_t
        // ceiling is enforced at the ONE increment site, obj_ref_inc, which refuses with
        // -KOS_EOVERFLOW rather than wrapping; there is deliberately no static_assert
        // welding it to MAX_THREADS x MAX_HANDLES.
        uint8_t sem_refs[KICKOS_MAX_SEMAPHORES] = {};
        // PI-mutex pool and its object-side refcount, same shape as the sems.
        SlotPool<Mutex, KICKOS_MAX_MUTEXES> mutexes;
        uint8_t mutex_refs[KICKOS_MAX_MUTEXES] = {};
        // Endpoint (IPC rendezvous) pool and its object-side refcount. recv_holders is NOT
        // here: its single home is the Endpoint struct, and it shares this ceiling because
        // one obj_ref_inc moves both counters or neither.
        SlotPool<Endpoint, KICKOS_MAX_ENDPOINTS> endpoints;
        uint8_t endpoint_refs[KICKOS_MAX_ENDPOINTS] = {};
        // Idle's TCB, the one thread the pool below does not seat. Placed against an
        // 8-aligned member so it introduces no fill of its own; its STACK is not here,
        // it comes from the arena (boot_stack_alloc).
        Thread idle_tcb;
        // Thread pool (see ThreadPool in thread.h): the TCBs + their kernel stacks,
        // intrinsic liveness (a slot is free iff state==EXITED), generation bumped at
        // reclaim (ABA). All allocation goes through thread_create_call().
        ThreadPool threads;
        // Memory-domain pool (see domain.h): shared region sets threads reference.
        // domains[0] = kernel domain, domains[1] = default-user (both immortal);
        // the rest are refcounted mem_base domains. All access via domain_*().
        Domain domains[KICKOS_MAX_DOMAINS];
        // Task pool (see task.h): the groups that hold those domains. No immortal slot
        // and no pinned index; a slot is free iff its refcount is 0 AND it has no creator.
        // All access via task_*().
        Task tasks[KICKOS_MAX_TASKS];

        // --- interrupt dispatch + IRQ-as-event bindings (irq.cc) ---
        IrqEntry irq_table[KICKOS_MAX_IRQ]; // line -> handler; ISR reads by index
        // Tier-1 bindings and their object-side refcount, same shape as the pools above.
        // Pooled, not bump-allocated, so a dead driver's line and slot come back:
        // irq_ref_drop detaches BEFORE it frees.
        SlotPool<IrqBinding, KICKOS_MAX_IRQ_HANDLES> irq_bindings;
        uint8_t irq_refs[KICKOS_MAX_IRQ_HANDLES] = {};
        uint32_t irq_spurious_count = 0; // IRQs on a line with no driver (masked)

    };

    // `task_holds` costs nothing only while it occupies the two bytes of padding that a
    // 32-bit target leaves before `sleepq`. Thread and Task pin their footprint with a
    // sizeof assert that fails the BUILD on every board; Kernel has no such assert, so a
    // field inserted on either side of `task_holds` would move the arena base and be caught
    // only by re-running a microbit capture and diffing .bss by hand, which is not routine.
    // This pins the adjacency the whole free-padding argument rests on. It does not prove
    // zero padding; it fails the moment the claim stops being checkable by inspection.
    // 32-BIT ONLY, and the assert caught that itself the first time it was written without the
    // guard: a 64-bit host aligns `sleepq` to 8, so six bytes follow `task_holds` there and the
    // adjacency is false by construction. The claim is about the boards. Same trap
    // task_scalar_bytes() documents for sizeof(Task). A host build prices the tail differently.
    static_assert(sizeof(void*) != 4
                      or offsetof(Kernel, sleepq)
                             == offsetof(Kernel, task_holds) + sizeof(Kernel::task_holds),
                  "task_holds no longer abuts sleepq on a 32-bit target, so the free-padding "
                  "claim needs re-measuring: microbit's arena base moves with Kernel's size");

    namespace detail
    {
        extern InstanceLocal<Kernel> g_instance;
    }

    // The single access seam for instance-scoped state.
    inline Kernel& kernel()
    {
        return detail::g_instance.get();
    }
}

#endif
