// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_THREAD_H
#define KICKOS_THREAD_H

#include <stdint.h>
#include <stddef.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h>

namespace kickos
{

    enum class ThreadState : uint8_t
    {
        INACTIVE, // not yet added
        READY,    // runnable, on a ready list
        RUNNING,  // currently executing
        BLOCKED,  // on a wait queue
        SLEEPING, // on the timer delta list
        EXITED    // done
    };

    enum class Policy : uint8_t
    {
        FIFO,
        RR
    };

    // The TCB. Intrusive links keep the scheduler allocation-free.
    struct Thread
    {
        arch_context ctx; // saved machine context (opaque)

        // ready-list / wait-queue membership (doubly linked, null-terminated)
        Thread* qnext;
        Thread* qprev;
        void* wait_queue; // WaitQueue* we are parked on, or nullptr

        // timer delta-list membership (singly linked, sorted by deadline)
        Thread* tnext;
        uint64_t deadline_ns;
        bool on_timer;

        char const* name;
        uint8_t prio;
        uint8_t base_prio; // for future priority inheritance
        Policy policy;
        ThreadState state;
        bool privileged;

        // Round-robin: quantum_ns == 0 means no slicing (pure FIFO within prio).
        uint32_t quantum_ns;
        uint64_t slice_deadline_ns;

        void* stack_base;
        size_t stack_size;

        arch_mpu_region regions[KICKOS_MPU_MAX_REGIONS];
        size_t region_count;

        intptr_t wait_result; // result handed to us by a waker

        uint64_t switch_count; // introspection
    };

    // Attributes for thread creation.
    struct ThreadAttr
    {
        char const* name = "thread";
        uint8_t prio = KICKOS_PRIO_MIN;
        Policy policy = Policy::FIFO;
        uint32_t quantum_ns = 0;
        bool privileged = true;
    };

}

#endif
