// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_THREAD_H
#define KICKOS_THREAD_H

#include <stdint.h>
#include <stddef.h>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/config.h>
#include <kickos/endpoint.h> // EP_SERVED_NONE: the served-endpoint chain head below
#include <kickos/list.h>

#include <kickos/sys/abi.h> // KOS_THREAD_NONE: the handle codec's reserved index

namespace kickos
{
    struct Domain; // kickos/domain.h: the shared region set a thread belongs to
    struct Mutex;  // kickos/sync.h: PI bookkeeping links (blocked_on + held list)

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

    // Call/reply state (M4.4). A thread is CALL_NONE unless it is mid-kos_call:
    // CALL_SEND_WAIT while parked on an endpoint's send_waiters before a receiver has
    // taken its request, CALL_REPLY_WAIT while parked queue-less waiting for the reply.
    // Zero == CALL_NONE, so the thread_create memset leaves a fresh TCB call-idle.
    enum CallState : uint8_t
    {
        CALL_NONE = 0,
        CALL_SEND_WAIT,
        CALL_REPLY_WAIT
    };

    // Kernel-owned bounded copy of a thread name (never aliases a user pointer).
    constexpr size_t KICKOS_THREAD_NAME_MAX = 16;

    // Per-Kernel monotonic thread identity (telemetry). idle is created first, so
    // idle == KICKOS_TID_IDLE; KICKOS_TID_NONE is the never-assigned "no thread"
    // sentinel. trace::TRACE_NO_THREAD aliases KICKOS_TID_NONE (record.h is a lower,
    // dependency-free layer that cannot see this header; ktrace.cc static_asserts
    // the two stay equal).
    constexpr uint16_t KICKOS_TID_NONE = 0xFFFF;
    constexpr uint16_t KICKOS_TID_IDLE = 0;

    // The TCB. Intrusive links keep the scheduler allocation-free.
    struct Thread
    {
        // EVERY member below needs an initialiser, holding exactly the value .bss zeroing
        // gives it: that is what keeps the implicit default constructor constexpr, and so
        // the static TCBs and the whole Kernel constinit. Drop one and kmain.cc and
        // instance.cc stop compiling.
        arch_context ctx{}; // saved machine context (opaque)

        // ready-list XOR wait-queue XOR reply-donor membership (shared node; see list.h)
        ListNode link;
        // wait queue we're parked on, or nullptr; read at sem_timedwait (Later)
        List* wait_queue = nullptr;

        // timer delta-list membership (singly linked, sorted by deadline); SEPARATE
        // from `link` so a timed wait can be on the timer list AND a wait queue at once.
        Thread* tnext = nullptr;
        uint64_t deadline_ns = 0;
        bool on_timer = false;

        // Per-Kernel monotonic thread identity (telemetry). Assigned in
        // thread_create; idle is created first so idle == 0. 0xFFFF is the
        // "no thread" sentinel (never assigned); 0 is idle-only after wrap.
        uint16_t id = 0;

        char name_buf[KICKOS_THREAD_NAME_MAX] = {}; // kernel-owned bounded copy; name points here
        char const* name = nullptr; // -> name_buf (set in thread_create); never a user pointer
        uint8_t prio = 0;      // EFFECTIVE priority: the only field sched/policy/wq read.
                               // Sole writer is sched::set_prio (re-seats READY threads).
        uint8_t base_prio = 0; // assignment anchor; PI raises `prio` above it, never below
        Policy policy = Policy::FIFO;
        ThreadState state = ThreadState::INACTIVE;
        bool privileged = false;
        // Set once at the top of exit_current, never cleared: this thread is running its
        // own capability teardown. The sweep RELEASES IrqLock between chunks, so `state`
        // cannot serve as the marker (a switch back in rewrites it to RUNNING). Gates the
        // cross-task reply mint, since a half-torn table must not accept a new cap, and the
        // wake-during-teardown switch deferral.
        bool dying = false;
        // CapAuthority (AUTH_*) bits. Read by cap_check_authority WITHOUT IrqLock, so it
        // must stay a single byte that no path writes concurrently: the parent seats it at
        // spawn before the child runs, and only the thread itself narrows it. Ignored when
        // `privileged`. Fits the padding before quantum_ns; moving it grows every TCB.
        uint8_t authority = 0;

        // Round-robin: quantum_ns == 0 means no slicing (pure FIFO within prio).
        uint32_t quantum_ns = 0;
        uint64_t slice_deadline_ns = 0;

        void* stack_base = nullptr;
        size_t stack_size = 0;
        // stack_base was demand-allocated by the kernel (convenient spawn) and must
        // be harvested onto the free list when this slot is reclaimed. A caller-owned
        // stack (app-supplied) is never harvested: the app owns that memory.
        bool kstack_owned = false;
        // Cancellation request (KOS_SYS_THREAD_KILL). One-way: set by the spawner, never
        // cleared, honoured only at the target's own cancellation points, of which irq_wait
        // is the only one. A thread that never reaches one keeps running.
        bool cancelled = false;
        // Who may cancel this thread: the KILL TAG of the thread that spawned it, or
        // KILL_TAG_NONE. It is the whole of the kill gate, so it must never alias; see
        // kill_tag_of and the clear in ThreadPool::alloc. ThreadPool::KILL_TAG_NONE is 0,
        // spelled out here because ThreadPool is declared below this struct.
        uint16_t spawner_tag = 0;
        // These three fit the padding before `domain`; moving them grows every TCB.

        // The memory domain this thread belongs to (shared region set + privilege).
        // Its regions are copied into regions[] below at create, plus this thread's
        // private stack; the effective set is what arch_mpu_apply loads per switch-in.
        Domain* domain = nullptr;
        arch_mpu_region regions[KICKOS_MPU_MAX_REGIONS] = {};
        size_t region_count = 0;

        intptr_t wait_result = 0; // wake-status channel (mutex: 0 / -KOS_EOWNERDEAD;
                                  // endpoint: byte count >= 0, or -KOS_EPIPE); the waker
                                  // writes it before sched::wake, the sleeper reads it
                                  // after wq_block returns. Timed wait shares it.

        // Parked-IPC descriptor: valid ONLY while this thread is parked on an endpoint
        // waitq (send_waiters/recv_waiters). The arriving peer reads it privileged under
        // the lock to copy into/out of this thread's buffer (bound-checked at park entry).
        struct IpcDesc
        {
            uintptr_t buf;       // this thread's own message buffer (already bound-checked)
            size_t    len;       // sender: bytes to send; receiver: buffer capacity
            uintptr_t badge_out; // receiver only: where to store the badge (0 => none)
        };
        IpcDesc ipc = {};

        // Call/reply descriptor (M4.4): valid while parked in a kos_call. call_rx_cap
        // is the reply capacity (in-place: the request buffer becomes the reply target);
        // call_seq is bumped per call and its low 8 bits ride the minted reply cap (the
        // late-reply ABA guard, one-shot); call_state is a CallState. Written by the
        // caller before parking and by the popper/replier under IrqLock (single-writer
        // at every stage, same discipline as ipc). Zeroed by the thread_create memset.
        size_t call_rx_cap = 0;
        uint16_t call_seq = 0;
        uint8_t call_state = CALL_NONE;

        // Priority-inheritance bookkeeping (M3 mutex). blocked_on is the mutex this
        // thread is parked on (nullptr otherwise), the chain-walk edge. held_list is
        // the head of the mutexes this thread OWNS, linked through Mutex::next_held;
        // thread_effective_prio scans it. Both are touched only under IrqLock at the
        // mutex block/unblock sites.
        Mutex* blocked_on = nullptr;
        Mutex* held_list = nullptr;

        // Callers parked in CALL_REPLY_WAIT on a reply cap THIS thread holds: one entry per
        // live CAP_REPLY in its table, linked through the caller's own `link`, which is free
        // because a reply-waiting caller is on no other list. The donor enumeration for
        // thread_effective_prio, which must NOT walk the cap table instead. The reply, close
        // and teardown arms all empty it, so a dying server never strands a donor.
        HeadList reply_waiters;

        uint64_t switch_count = 0; // introspection

        // Per-task capability table (cap.h). The run is reserved from the slab by the CALLER
        // before thread_create, as `domain` is, so an exhausted slab fails the spawn instead
        // of leaving a half-built thread. thread_create's memset would zero the directory, so
        // it re-establishes it from ThreadAttr afterwards.
        //
        // Every scan site must be bounded by thread_cap_capacity and never by
        // KICKOS_MAX_HANDLES: capacities differ per task, a capacity of 0 is legal, and
        // cap_run_held (cap.h) enumerates the threads that have none.
        CapRun caps = {};
        // Head of the run's free-slot list (cap.h), a slot index biased by one. KCAP_FREE_NONE
        // is 0, so the thread_create memset leaves it EMPTY and not "slot 0": a thread whose
        // list was never threaded refuses every mint rather than handing out a reserved index.
        uint16_t cap_free_head = KCAP_FREE_NONE;
#if KCAP_RUN_CHUNKS > 1
        // This task's addressable capacity, 0 when it holds no run. Root's is
        // KICKOS_MAX_HANDLES and a spawned child's is narrower, so no reader may assume
        // either.
        uint16_t cap_width = 0;
        // Live inbound CAP_REPLY entries, bounded by KICKOS_CAP_REPLY_MAX.
        //
        // Both fields land in the TAIL PADDING the chunk directory's second pointer creates,
        // so they cost nothing here and 8 B per TCB on the flat path, which the 16 KiB boards
        // cannot spare. Keep the uint16_t group CONTIGUOUS after `caps`.
        uint16_t cap_reply_live = 0;
#endif
        // Endpoints where ep->server == this thread, chained through Endpoint::next_served
        // (endpoint.h). The SEND_WAIT donor enumeration for thread_effective_prio, which runs
        // interrupt-masked and so may walk neither the capability table nor the endpoint pool.
        // EP_SERVED_NONE is 0, so the thread_create memset leaves it empty.
        uint16_t served_head = EP_SERVED_NONE;
    };

    // A thread's capability-table capacity: the width it was seated with if it holds a run,
    // else 0.
    inline uint32_t thread_cap_capacity(Thread const* t)
    {
#if KCAP_RUN_CHUNKS == 1
        // The flat run is exactly one chunk of exactly KICKOS_MAX_HANDLES slots, so a held
        // run IS the ceiling and there is nothing to store.
        if (not cap_run_held(t->caps))
        {
            return 0;
        }
        return KICKOS_MAX_HANDLES;
#else
        return t->cap_width; // 0 == no run: attach and detach keep the two in step
#endif
    }

    // Return t's run to the slab and clear everything that travels with it. A width left
    // naming a run this call gave away answers thread_cap_capacity for a table that no
    // longer exists; a reply count left standing makes the slot's next occupant refuse its
    // first caller. Caller holds IrqLock.
    inline void thread_cap_release(Thread* t)
    {
#if KCAP_RUN_CHUNKS == 1
        uint16_t width = 0;
        cap_slab_detach(&t->caps, &t->cap_free_head, &width);
#else
        cap_slab_detach(&t->caps, &t->cap_free_head, &t->cap_width);
        t->cap_reply_live = 0;
#endif
    }

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
        // Default false: an attr struct that forgets the field must not mint privilege.
        // idle is the one thread that spells `true` out (kernel/init/kmain.cc).
        bool privileged = false;
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
        // Who is allowed to cancel the new thread (a kill tag). thread_spawn seats the
        // caller's; the boot TCBs (idle, root) leave it NONE and are so un-killable.
        uint16_t spawner_tag = 0;
        // The stack passed to thread_create was demand-allocated by the kernel and is
        // owned by the free list (harvest at reclaim). false for caller-owned and the
        // static idle/root stacks.
        bool kstack_owned = false;
        // Pre-reserved capability run (cap_slab_attach): an exhausted slab must fail the
        // spawn BEFORE anything is built. An empty directory is legal and means the thread
        // holds no capabilities. The free-list head and the seated capacity travel with it:
        // attach threads the list, and a run seated without its head would refuse every mint.
        // Unconditional, unlike Thread's: this is caller stack, not per-TCB .bss.
        CapRun cap_run = {};
        uint16_t cap_free_head = KCAP_FREE_NONE;
        uint16_t cap_width = 0;
    };

    // Static thread-slot pool (instance-scoped; the TCBs only, since default stacks are
    // demand-allocated from the arena). Bump-allocated, then EXITED slots reclaimed at spawn.
    // Liveness is INTRINSIC: a slot is free iff its TCB state is EXITED, so there is no
    // `used[]` bit to drift out of sync. The per-slot generation bumps at RECLAIM and not at
    // exit, so a handle to a just-exited-but-not-yet-reused slot still gen-matches; reuse
    // invalidates it (ABA). This is NOT the generic SlotPool: different liveness, generation
    // timing and reclaim. Caller serializes (IrqLock); reuse is safe because thread_create
    // re-inits the TCB, privilege posture included, from scratch.
    struct ThreadPool
    {
        // The uint16_t generation takes the other 16, so the handle spends the whole word: a
        // fully aged one has bit 31 set and no sign test says anything about it. The kill tag
        // below, not this, is what caps the pool at 65534.
        static constexpr int INDEX_BITS = 16;
        // STRICTLY less: the all-ones index is reserved and never seated, which is what makes
        // KOS_THREAD_NONE unmintable by ANY generation and not merely out of the current
        // pool's range. KILL_TAG_BOOT already forces a stricter bound, so this is the
        // backstop for a change to the tag scheme.
        static_assert(KICKOS_MAX_THREADS < (1 << INDEX_BITS),
                      "thread handle index field too small for KICKOS_MAX_THREADS, or the "
                      "pool would seat the index KOS_THREAD_NONE reserves");
        static_assert((KOS_THREAD_NONE & ((1u << INDEX_BITS) - 1u)) == ((1u << INDEX_BITS) - 1u),
                      "KOS_THREAD_NONE must carry the reserved all-ones index");

        // Kill-gate identity, DERIVED from the slot index rather than stored. The two boot
        // TCBs (idle, root) SHARE KILL_TAG_BOOT because neither is a slot, which is safe
        // only while idle issues no syscall (its body is a bare arch_idle_wait loop, see
        // kernel/init/kmain.cc). An idle that could ever CALL needs a tag of its own.
        static constexpr uint16_t KILL_TAG_NONE = 0;
        static constexpr uint16_t KILL_TAG_BOOT = 0xFFFFu;
        static_assert(KICKOS_MAX_THREADS < KILL_TAG_BOOT,
                      "a pool slot's kill tag would collide with the boot tag");

        static constexpr uint16_t kill_tag_for_index(int index)
        {
            return static_cast<uint16_t>(index + 1);
        }

        Thread slots[KICKOS_MAX_THREADS];
        int next = 0;
        uint16_t gen[KICKOS_MAX_THREADS] = {};

        // A CAP_REPLY carries this handle in CapEntry::obj, whole and unmasked (cap.h). At
        // 16 + 16 the fit is EXACT: a widening would truncate the GENERATION from the top and
        // collapse cap_reply_caller's late-reply guard with nothing at runtime to report it.
        static_assert(INDEX_BITS + 8 * sizeof(gen[0]) <= 8 * sizeof(CapEntry::obj),
                      "a thread handle no longer fits CAP_REPLY's obj word");

        // Free list of reclaimed kernel-default stacks: a SINGLE size class
        // (KICKOS_USER_STACK_SIZE), so it cannot fragment and the link lives in the dead
        // block itself with no side table. A block enters the list only at the exited-slot
        // reclaim point (alloc, below), where its former owner is provably off-CPU.
        void* stack_free_list = nullptr;
#if KICKOS_HAVE_MPU
        // A demand-allocated stack is granted as ONE MPU region. Power of two is a
        // conservative compile-time superset: PMSAv7/NAPOT require it, the base+limit
        // backends would accept any granule multiple. The granule is a runtime seam
        // value, so the looser rule cannot be asserted here.
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
        //
        // Lowest-exited, NOT SlotPool's next-fit cursor: this holds `next` down, and both the
        // scan below and the spawner_tag sweep are bounded by `next` under the caller's
        // IrqLock. The price is wrap distance, concentrated on one slot when one thread lives
        // at a time.
        [[nodiscard]] int alloc()
        {
            for (int s = 0; s < next; s++)
            {
                if (slots[s].state == ThreadState::EXITED)
                {
                    // Harvest the exited thread's kernel-allocated stack at the RECLAIM
                    // point, not at exit: only by now is the thread provably off-CPU
                    // (invariant exit-parks-for-deferred-switch, sched.cc), so writing the
                    // free-list link into its stack cannot race the final context save.
                    // Ownership moves to the list so a later reclaim of this same slot, for
                    // instance after a release(), never double-pushes the block.
                    if (slots[s].kstack_owned)
                    {
                        stack_push(slots[s].stack_base);
                        slots[s].kstack_owned = false;
                    }
                    // Same reclaim-point reasoning as the stack: the thread is off-CPU and
                    // cap_teardown has already emptied every entry. cap_slab_detach clears the
                    // directory as it returns each chunk, so a second reclaim of this slot
                    // cannot double-free one, and it clears the free-list head, which by then
                    // names a slot in a chunk this call gives away.
                    thread_cap_release(&slots[s]);
                    // A slot's kill tag is its INDEX and so outlives its occupant: a child
                    // still naming this tag must be orphaned before the slot changes hands,
                    // or the new occupant inherits cancel authority over threads it never
                    // spawned. Reuse is the only event that makes the tag ambiguous, which is
                    // why this is here and not at exit.
                    for (int j = 0; j < next; j++)
                    {
                        if (slots[j].spawner_tag == kill_tag_for_index(s))
                        {
                            slots[j].spawner_tag = KILL_TAG_NONE;
                        }
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
        // hole alloc() would never revisit, so it mirrors alloc()'s two cases:
        //   * a reclaimed EXITED slot: alloc() bumped its generation to invalidate the
        //     prior occupant's handle, but no reuse happened, so revert that bump (the
        //     prior occupant's join-by-handle must still resolve) and leave it EXITED,
        //     still reclaimable. Its stack, if any, was already harvested by alloc, which
        //     is correct regardless of this spawn's fate; kstack_owned is now false.
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

        // Index of a TCB in this pool, or -1 if it is not a pool slot (e.g. the
        // file-static root/idle TCBs). UB-free: compares addresses as integers rather
        // than subtracting pointers that may not point into slots[].
        int index_of(Thread const* t) const
        {
            uintptr_t const base = reinterpret_cast<uintptr_t>(&slots[0]);
            uintptr_t const p = reinterpret_cast<uintptr_t>(t);
            if (p < base)
            {
                return -1;
            }
            uintptr_t const off = p - base;
            if (off >= sizeof(slots))
            {
                return -1;
            }
            if (off % sizeof(Thread) != 0)
            {
                return -1; // interior pointer, not a slot base
            }
            return static_cast<int>(off / sizeof(Thread));
        }

        // The kill tag naming `t`: its slot index if it is one of ours, else the shared
        // boot tag. Never KILL_TAG_NONE, so a thread with no spawner matches nobody.
        uint16_t kill_tag_of(Thread const* t) const
        {
            int const index = index_of(t);
            if (index < 0)
            {
                return KILL_TAG_BOOT;
            }
            return kill_tag_for_index(index);
        }

        // The opaque handle for a live slot index, carrying its current generation.
        kos_thread_t handle_for(int index) const
        {
            return (static_cast<uint32_t>(gen[index]) << INDEX_BITS) |
                   static_cast<uint32_t>(index);
        }
    };
}

#endif
