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
    struct Domain; // kickos/domain.h -- the shared region set a thread belongs to

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

        // Per-Kernel monotonic thread identity (telemetry). Assigned in
        // thread_create; idle is created first so idle == 0. 0xFFFF is the
        // "no thread" sentinel (never assigned); 0 is idle-only after wrap.
        uint16_t id;

        char name_buf[16];  // kernel-owned bounded copy; name points here
        char const* name;   // -> name_buf (set in thread_create); never a user pointer
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

        // The memory domain this thread belongs to (shared region set + privilege).
        // Its regions are copied into regions[] below at create, plus this thread's
        // private stack; the effective set is what arch_mpu_apply loads per switch-in.
        Domain* domain;
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
        // Pre-resolved domain (thread_spawn sets this so pool exhaustion fails the
        // spawn cleanly). null => thread_create resolves from privileged + mem_base.
        Domain* domain = nullptr;
    };

    // Static thread-slot pool (instance-scoped; the TCBs + their kernel stacks). Bump-
    // allocated, then EXITED slots reclaimed at spawn. Liveness is INTRINSIC -- a slot is
    // free iff its TCB state is EXITED, the scheduler's single source of truth; there is
    // deliberately no `used[]` bit to drift out of sync. The per-slot generation bumps at
    // RECLAIM, not at exit, so a handle to a just-exited-but-not-yet-reused slot still
    // gen-matches (a future join-by-handle can read its result); reuse invalidates it
    // (ABA). Tailored on purpose -- this is NOT the generic SlotPool (different liveness,
    // generation timing, and reclaim). Caller serializes (IrqLock); reuse is safe because
    // thread_create re-inits the TCB (incl. privilege posture) from scratch.
    struct ThreadPool
    {
        static constexpr int kIndexBits = 8; // handle low bits; the generation takes the rest
        static_assert(KICKOS_MAX_THREADS <= (1 << kIndexBits),
                      "thread handle index field too small for KICKOS_MAX_THREADS");

        Thread slots[KICKOS_MAX_THREADS];
        alignas(16) unsigned char stacks[KICKOS_MAX_THREADS][KICKOS_USER_STACK_SIZE];
        int next = 0;
        uint16_t gen[KICKOS_MAX_THREADS] = {};

        // Claim a slot: reclaim an EXITED one (bumping its generation to kill stale
        // handles) or bump-allocate a fresh one. Returns the index, or -1 if full.
        [[nodiscard]] int alloc()
        {
            for (int s = 0; s < next; s++)
            {
                if (slots[s].state == ThreadState::EXITED)
                {
                    gen[s]++;
                    return s;
                }
            }
            if (next >= KICKOS_MAX_THREADS)
            {
                return -1;
            }
            return next++;
        }

        // The opaque handle for a live slot index, carrying its current generation.
        int handle_for(int index) const
        {
            return static_cast<int>((static_cast<uint32_t>(gen[index]) << kIndexBits) |
                                    static_cast<uint32_t>(index));
        }
    };
}

#endif
