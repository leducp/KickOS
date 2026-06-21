// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_THREAD_H
#define KICKOS_THREAD_H

#include <stdint.h>
#include <stddef.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h>
#include <kickos/list.h>

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

        // ready-list XOR wait-queue membership (shared intrusive node; see list.h)
        ListNode link;
        List* wait_queue; // wait queue we're parked on, or nullptr; read at sem_timedwait (Later)

        // timer delta-list membership (singly linked, sorted by deadline); SEPARATE
        // from `link` so a timed wait can be on the timer list AND a wait queue at once.
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

        intptr_t wait_result; // reserved for timed wait (unused today)

        uint64_t switch_count; // introspection
    };

    // Recover the TCB owning a ready/wait list node (nullptr-safe).
    inline Thread* thread_of(ListNode* n)
    {
        if (n == nullptr)
        {
            return nullptr;
        }
        return KICKOS_CONTAINER_OF(n, Thread, link);
    }

    // Attributes for thread creation.
    struct ThreadAttr
    {
        char const* name = "thread";
        uint8_t prio = KICKOS_PRIO_MIN;
        Policy policy = Policy::FIFO;
        uint32_t quantum_ns = 0;
        bool privileged = true;
        // Optional domain data region granted to an unprivileged thread (RW).
        // Threads sharing one region share a memory domain; base==0 => none.
        void* mem_base = nullptr;
        size_t mem_size = 0;
    };
}

#endif
