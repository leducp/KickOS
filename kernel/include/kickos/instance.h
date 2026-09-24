// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The instance-scoped kernel runtime core. Several kernel instances may co-reside in one
// host process, one per simulated MCU, so nothing here may become a file-static. App-owned
// objects (TCBs, semaphores) stay caller-owned; this holds only the runtime's own bookkeeping.
// The sim arch backend keeps its own parallel SimInstance and never crosses the arch seam.
//
// State a module owns privately stays in that module and wraps in InstanceLocal
// (instance_local.h) rather than moving here.

#ifndef KICKOS_INSTANCE_H
#define KICKOS_INSTANCE_H

#include <stdint.h>
#include <stddef.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/endpoint.h>
#include <kickos/instance_local.h>
#include <kickos/irq.h>
#include <kickos/list.h>
#include <kickos/notify.h>
#include <kickos/slotpool.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/sync.h>

namespace kickos
{
    struct SchedPolicy;

    // Which core within this kernel is running: a slot index in [0, KICKOS_KERNEL_CORES).
    // Under AMP arch_cpu_id() answers 1..3 on a peer while this answers 0, so current[],
    // idle[] and boot[] take this index and never the core's machine id.
    // tests/static/check_kernel_core_index.sh refuses the other subscript.
#if KICKOS_MULTICORE_MODEL_SHARED
#define kickos_kernel_core() arch_cpu_id()
#else
#define kickos_kernel_core() 0u
#endif

    struct Kernel
    {
        // --- scheduler mechanism (sched.cc) ---
        // One set per core. A thread sits on exactly one core's structure, named by its
        // queue_core, so a core picks without consulting any peer's state. At one kernel
        // core the outer extent is 1 and every index folds to a constant.
        List ready[KICKOS_KERNEL_CORES][KICKOS_NUM_PRIO]; // per priority; running at front
        uint32_t ready_bitmap[KICKOS_KERNEL_CORES] = {};  // bit p set iff ready[c][p] non-empty
        Thread* current[KICKOS_KERNEL_CORES] = {}; // indexed by kickos_kernel_core()
        Thread* idle[KICKOS_KERNEL_CORES] = {}; // indexed by kickos_kernel_core()
        unsigned live = 0; // non-idle threads not yet EXITED
#if KICKOS_KERNEL_CORES > 1
        // Bit c of push_asked[h]: core c asks holder h to push it a thread. seated_prio[c]: the
        // priority core c's last pass seated, left high across a lowering so that pass sees
        // the drop.
        uint32_t push_asked[KICKOS_KERNEL_CORES] = {};
        uint8_t seated_prio[KICKOS_KERNEL_CORES] = {};
#endif
        arch_context boot[KICKOS_KERNEL_CORES] = {}; // indexed by kickos_kernel_core()
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

        // Tasks currently holding a creator hold (task.cc). Declared here, away from the
        // task pool below, because the two bytes before `sleepq` are padding on every
        // 32-bit target regardless of telemetry posture: microbit's `_ebss` is its arena
        // base, so a byte that grows the struct costs a whole allocation granule.
        uint16_t task_holds = 0;

        // --- tickless time (time.cc) ---
        Thread* sleepq = nullptr; // sorted ascending by deadline_ns
        // What each core's one-shot comparator is programmed for, and the sole authority for
        // it. UINT64_MAX means disarmed. Zero-initialised deliberately: 0 is a value no
        // computation here can produce (arm_slice answers UINT64_MAX for a FIFO thread,
        // ktime_sleep_until floors at now + MIN_DELTA), so the first rearm always programs
        // whatever a bring-up probe left in the hardware.
        uint64_t timer_armed_ns[KICKOS_KERNEL_CORES] = {};

        // --- syscall object pools (syscall.cc) ---
        // Semaphore registry: a generational slot pool (see slotpool.h). Reached only
        // through its own resolve(); the generation wraps every 2^16 destroys of one slot.
        SlotPool<Semaphore, KICKOS_MAX_SEMAPHORES> sems;
        // Object-side refcount owned by the cap layer: how many caps name slot i. alloc
        // sets 1, delegate bumps, close decrements, and 0 frees the slot. The uint8_t
        // ceiling is enforced at the one increment site, obj_ref_inc, which refuses with
        // -KOS_EOVERFLOW rather than wrapping.
        uint8_t sem_refs[KICKOS_MAX_SEMAPHORES] = {};
        // PI-mutex pool and its object-side refcount, same shape as the sems.
        SlotPool<Mutex, KICKOS_MAX_MUTEXES> mutexes;
        uint8_t mutex_refs[KICKOS_MAX_MUTEXES] = {};
        // Endpoint (IPC rendezvous) pool and its object-side refcount. recv_holders lives
        // solely in the Endpoint struct, and shares this ceiling because one obj_ref_inc
        // moves both counters or neither.
        SlotPool<Endpoint, KICKOS_MAX_ENDPOINTS> endpoints;
        uint8_t endpoint_refs[KICKOS_MAX_ENDPOINTS] = {};
        // Idle's TCB; the thread pool below never seats it. Placed against an 8-aligned
        // member so it introduces no fill of its own; its stack comes from the arena
        // (boot_stack_alloc).
        Thread idle_tcb;
#if KICKOS_KERNEL_CORES > 1
        // The other cores' idle TCBs. Outside the thread pool, exactly as idle_tcb is.
        Thread idle_tcb_peer[KICKOS_KERNEL_CORES - 1];
#endif
        // Thread pool (see ThreadPool in thread.h): the TCBs + their kernel stacks,
        // intrinsic liveness (a slot is free iff state==EXITED), generation bumped at
        // reclaim (ABA). All allocation goes through thread_create_call().
        ThreadPool threads;
        // Memory-domain pool (see domain.h): shared region sets threads reference.
        // domains[0] = kernel domain, domains[1] = default-user (both immortal);
        // the rest are refcounted mem_base domains. All access via domain_*().
        Domain domains[KICKOS_MAX_DOMAINS];
        // Task pool (see task.h): the groups that hold those domains. A slot is free iff its
        // refcount is 0 and it has no creator. All access via task_*().
        Task tasks[KICKOS_MAX_TASKS];
#if KICKOS_HAVE_ASPACE
        // Frame-run pool and its object-side refcount, same shape as the pools above. Only a
        // translating board has a frame pool to name, so this is the one kind whose storage
        // is posture-gated.
        SlotPool<FrameRun, KICKOS_MAX_FRAME_RUNS> frame_runs;
        uint8_t frame_run_refs[KICKOS_MAX_FRAME_RUNS] = {};
#endif

        // --- interrupt dispatch + IRQ-as-event bindings (irq.cc) ---
        IrqEntry irq_table[KICKOS_MAX_IRQ]; // line -> handler; ISR reads by index
        // Tier-1 bindings and their object-side refcount, same shape as the pools above.
        // Pooled, not bump-allocated, so a dead driver's line and slot come back:
        // irq_ref_drop detaches before it frees.
        SlotPool<IrqBinding, KICKOS_MAX_IRQ_HANDLES> irq_bindings;
        uint8_t irq_refs[KICKOS_MAX_IRQ_HANDLES] = {};
        uint32_t irq_spurious_count = 0; // IRQs on a line with no driver (masked)

        // --- notification objects (notify.cc) ---
        // The object a driver waits on, and the object an IRQ binding signals. Same shape as
        // the pools above; the refcount counts every capability naming a slot, the bind, and
        // each attached IRQ binding.
        SlotPool<Notification, KICKOS_MAX_NOTIFY> notifies;
        uint8_t notify_refs[KICKOS_MAX_NOTIFY] = {};

    };

    // `task_holds` costs nothing only while it occupies the two bytes of padding that a
    // 32-bit target leaves before `sleepq`; this pins that adjacency. 32-bit only: a 64-bit
    // host aligns `sleepq` to 8, so six bytes follow `task_holds` there.
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

    // A hold set is 32 bits of pool slots (TaskObjectHolds, cap.h), so a wider pool would
    // carry slots no ceiling can see. The Kconfig ranges are narrowed to match; this is the
    // backstop for a board_config.h that defines a width directly.
    static_assert(KICKOS_MAX_SEMAPHORES <= 32 and KICKOS_MAX_MUTEXES <= 32
                      and KICKOS_MAX_ENDPOINTS <= 32 and KICKOS_MAX_IRQ_HANDLES <= 32
                      and KICKOS_MAX_NOTIFY <= 32,
                  "a charged object pool is wider than a hold set's 32 bits: narrow the pool, "
                  "or widen TaskObjectHolds in cap.h and this assert together");

    // How many pool slots a hold set names. Costs one iteration per set bit, so a task
    // holding nothing pays nothing.
    inline int task_object_count(uint32_t held)
    {
        int n = 0;
        while (held != 0)
        {
            held = held & (held - 1u);
            n++;
        }
        return n;
    }
}

#endif
