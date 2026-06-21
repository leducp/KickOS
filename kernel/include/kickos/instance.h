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
#include <kickos/irq.h>
#include <kickos/list.h>
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

        // --- tickless time (time.cc) ---
        Thread* sleepq = nullptr; // sorted ascending by deadline_ns

        // --- syscall object pools (syscall.cc) ---
        // Semaphore freelist: sem_used marks live slots; sem_gen is bumped on
        // destroy so a stale handle (index+gen) fails to resolve rather than
        // aliasing a recycled slot. All access goes through sem_resolve(). (gen
        // wraps every 2^16 destroys of one slot -- acceptable until the M2 handle
        // table subsumes it.)
        Semaphore sems[KICKOS_MAX_SEMAPHORES];
        bool sem_used[KICKOS_MAX_SEMAPHORES];
        uint16_t sem_gen[KICKOS_MAX_SEMAPHORES];
        Thread thread_pool[KICKOS_MAX_THREADS];
        alignas(16) unsigned char thread_stacks[KICKOS_MAX_THREADS][KICKOS_USER_STACK_SIZE];
        int thread_next = 0;

        // --- interrupt dispatch + IRQ-as-event bindings (irq.cc) ---
        IrqEntry irq_table[KICKOS_MAX_IRQ]; // line -> handler; ISR reads by index
        IrqBinding irq_bindings[KICKOS_MAX_IRQ_HANDLES];
        int irq_binding_count = 0;
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
