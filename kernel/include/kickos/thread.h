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
        // stack_base was demand-allocated by the kernel (convenient spawn) and must
        // be harvested onto the free list when this slot is reclaimed. A caller-owned
        // stack (app-supplied) is never harvested -- the app owns that memory.
        bool kstack_owned;

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
        // Optional device/MMIO region granted to an unprivileged thread (R|W|DEV,
        // never executable). Privileged-only at the spawn boundary; a domain that
        // carries one is a capability and is never shared. base==0 => none.
        void* mmio_base = nullptr;
        size_t mmio_size = 0;
        // Pre-resolved domain (thread_spawn sets this so pool exhaustion fails the
        // spawn cleanly). null => thread_create resolves from privileged + mem_base.
        Domain* domain = nullptr;
        // The stack passed to thread_create was demand-allocated by the kernel and is
        // owned by the free list (harvest at reclaim). false for caller-owned and the
        // static idle/root stacks.
        bool kstack_owned = false;
    };

    // Static thread-slot pool (instance-scoped; the TCBs only -- default stacks are
    // demand-allocated from the arena, not pre-reserved here). Bump-allocated, then
    // EXITED slots reclaimed at spawn. Liveness is INTRINSIC -- a slot is free iff its
    // TCB state is EXITED, the scheduler's single source of truth; there is deliberately
    // no `used[]` bit to drift out of sync. The per-slot generation bumps at RECLAIM,
    // not at exit, so a handle to a just-exited-but-not-yet-reused slot still gen-matches
    // (a future join-by-handle can read its result); reuse invalidates it (ABA). Tailored
    // on purpose -- this is NOT the generic SlotPool (different liveness, generation
    // timing, and reclaim). Caller serializes (IrqLock); reuse is safe because
    // thread_create re-inits the TCB (incl. privilege posture) from scratch.
    struct ThreadPool
    {
        static constexpr int kIndexBits = 8; // handle low bits; the generation takes the rest
        static_assert(KICKOS_MAX_THREADS <= (1 << kIndexBits),
                      "thread handle index field too small for KICKOS_MAX_THREADS");

        Thread slots[KICKOS_MAX_THREADS];
        int next = 0;
        uint16_t gen[KICKOS_MAX_THREADS] = {};

        // Free list of reclaimed kernel-default stacks -- a SINGLE size class
        // (KICKOS_USER_STACK_SIZE), so no fragmentation and the link needs no side
        // table: it is stored in the dead block itself. A block enters the list only
        // at the exited-slot reclaim point (alloc, below), where its former owner is
        // provably off-CPU. FUTURE (M4): a general multi-size-class freeing allocator
        // (for arch_ram_alloc at large) subsumes this; this is the special case for
        // default thread stacks only.
        void* stack_free_list = nullptr;
#if KICKOS_HAVE_MPU
        // A demand-allocated stack is granted as ONE MPU region, so its size must be a
        // power of two the descriptor can name (PMSA/NAPOT); arch_ram_alloc then hands
        // out a naturally-aligned block, a valid region base.
        static_assert((KICKOS_USER_STACK_SIZE & (KICKOS_USER_STACK_SIZE - 1)) == 0,
                      "KICKOS_USER_STACK_SIZE must be a power of two under MPU enforcement");
#endif
        static_assert(KICKOS_USER_STACK_SIZE >= sizeof(void*),
                      "a reclaimed stack block must be able to hold the free-list link");

        void stack_push(void* block)
        {
            *reinterpret_cast<void**>(block) = stack_free_list;
            stack_free_list = block;
        }
        void* stack_pop()
        {
            void* block = stack_free_list;
            if (block != nullptr)
            {
                stack_free_list = *reinterpret_cast<void**>(block);
            }
            return block;
        }

        // Claim a slot: reclaim an EXITED one (bumping its generation to kill stale
        // handles) or bump-allocate a fresh one. Returns the index, or -1 if full.
        [[nodiscard]] int alloc()
        {
            for (int s = 0; s < next; s++)
            {
                if (slots[s].state == ThreadState::EXITED)
                {
                    // Harvest the exited thread's kernel-allocated stack HERE, at the
                    // reclaim point -- not at exit. By now the exited thread is provably
                    // off-CPU (invariant exit-parks-for-deferred-switch, sched.cc: it
                    // parked in exit_current until its switch-away committed), so writing
                    // the free-list link into its stack cannot race the final context
                    // save. Move ownership to the list (clear the flag) so a later
                    // reclaim of this same slot -- e.g. after a release() -- never
                    // double-pushes the block.
                    if (slots[s].kstack_owned)
                    {
                        stack_push(slots[s].stack_base);
                        slots[s].kstack_owned = false;
                    }
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

        // Undo a slot claimed by alloc() when the spawn fails AFTER the claim (e.g. the
        // arena has no stack to give). Must NOT burn a generation and must NOT leave a
        // hole alloc() would never revisit -- so it mirrors alloc()'s two cases:
        //   * a reclaimed EXITED slot: alloc() bumped its generation to invalidate the
        //     prior occupant's handle, but no reuse happened, so revert that bump (the
        //     prior occupant's join-by-handle must still resolve) and leave it EXITED,
        //     still reclaimable. (Its stack, if any, was already harvested by alloc --
        //     correct regardless of this spawn's fate; kstack_owned is now false.)
        //   * a fresh bump slot (INACTIVE, from zero-init, always the last one under the
        //     spawn lock): un-bump `next`, else it becomes a permanent hole (alloc only
        //     ever reclaims EXITED, never INACTIVE).
        void release(int i)
        {
            if (slots[i].state == ThreadState::EXITED)
            {
                gen[i]--;
                return;
            }
            if (i == next - 1)
            {
                next--;
            }
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
