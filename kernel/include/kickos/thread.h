// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_THREAD_H
#define KICKOS_THREAD_H

#include <stdint.h>
#include <stddef.h>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/config.h>
#include <kickos/endpoint.h> // EP_SERVED_NONE
#include <kickos/list.h>
#include <kickos/mpuset.h>

#include <kickos/sys/abi.h> // KOS_THREAD_NONE
#include <kickos/sys/atomic.h>

namespace kickos
{
    struct Endpoint; // kickos/endpoint.h
    struct Mutex;    // kickos/sync.h
    struct Task;     // kickos/task.h

    enum class ThreadState : uint8_t
    {
        INACTIVE, // not yet added
        READY,    // on a ready list
        RUNNING,
        BLOCKED,  // parked: on a wait queue, on the timer delta list, or on neither
        EXITED
    };

    enum class Policy : uint8_t
    {
        FIFO,
        RR
    };

    // A thread is CALL_NONE unless it is mid-kos_call: CALL_SEND_WAIT while parked on an
    // endpoint's send_waiters before a receiver has taken its request, CALL_REPLY_WAIT while
    // parked queue-less waiting for the reply. Zero == CALL_NONE, so the thread_create kmemset
    // leaves a fresh TCB call-idle.
    enum CallState : uint8_t
    {
        CALL_NONE = 0,
        CALL_SEND_WAIT,
        CALL_REPLY_WAIT
    };

    // What a parked thread waits FOR, and the object owning the list it is on. Set at every park
    // and cleared at every unpark: thread_kill dispatches on it, and a mis-tagged park unwinds the
    // wrong list. WAIT_JOIN, WAIT_LIVE_LAST and WAIT_TASK_EMPTY are on no list at all, so the tag
    // is the only thing that finds them.
    enum WaitKind : uint8_t
    {
        WAIT_NONE = 0,
        WAIT_MUTEX,     // wait_obj: the Mutex. The PI chain-walk edge.
        WAIT_SEM,       // wait_obj: the Semaphore
        WAIT_IRQ,       // wait_obj: the IrqBinding. A sem queue; this park reads wait_result.
        WAIT_EP_SEND,   // wait_obj: the Endpoint; on its send_waiters
        WAIT_EP_RECV,   // wait_obj: the Endpoint; on its recv_waiters
        WAIT_EP_REPLY,  // wait_obj: the SERVER thread; queue-less on its reply_waiters
        WAIT_SLEEP,     // wait_obj: none; on the timer delta list
        WAIT_JOIN,      // wait_obj: the TARGET thread; queue-less on no list at all
        WAIT_LIVE_LAST,  // wait_obj: none; queue-less. Carries no deadline, ever.
        WAIT_TASK_EMPTY, // wait_obj: the TASK; queue-less on no list at all
    };

    // Has this thread been asked to die, and how. Zero == CANCEL_NONE, so the thread_create
    // kmemset leaves a fresh TCB un-cancelled. Test against CANCEL_NONE and never for one kind:
    // the death point (kernel/syscall/syscall.cc) and the re-block refusal (kernel/irq/irq.cc)
    // are total over the non-zero values.
    enum CancelKind : uint8_t
    {
        CANCEL_NONE = 0,
        CANCEL_KILL = 1, // cooperative: dies at its next syscall entry, keeps its window
        // Its resume is claimed: it executes no further unprivileged instruction, since
        // switch_to rebuilds the incoming context before arch_switch.
        CANCEL_SLAY = 2
    };

    // Kernel-owned bounded copy of a thread name (never aliases a user pointer).
    constexpr size_t KICKOS_THREAD_NAME_MAX = 16;

    // Per-Kernel monotonic thread identity (telemetry). idle is created first, so
    // idle == KICKOS_TID_IDLE; KICKOS_TID_NONE is the never-assigned "no thread" sentinel.
    // trace::TRACE_NO_THREAD aliases KICKOS_TID_NONE and ktrace.cc static_asserts the two
    // stay equal.
    constexpr uint16_t KICKOS_TID_NONE = 0xFFFF;
    constexpr uint16_t KICKOS_TID_IDLE = 0;

    struct Thread
    {
        // EVERY member below needs an initialiser holding exactly the value .bss zeroing gives
        // it, or the implicit default constructor stops being constexpr and the constinit idle
        // TCB and Kernel (kmain.cc, instance.cc) stop compiling.
        arch_context ctx{}; // saved machine context (opaque)

        // ready-list XOR wait-queue XOR reply-donor membership (shared node; see list.h)
        ListNode link;
        // The wait queue we are parked on, or nullptr. Null does NOT mean "not parked": a
        // WAIT_EP_REPLY caller is parked queue-less, and wait_kind below is its only edge.
        List* wait_queue = nullptr;

        // timer delta-list membership (singly linked, sorted by deadline); SEPARATE
        // from `link` so a timed wait can be on the timer list AND a wait queue at once.
        Thread* tnext = nullptr;

        // Where uint64_t aligns to 8 this is the padding word before deadline_ns; moving it
        // grows every TCB. Advanced by switch_to for the INCOMING thread, read by
        // wq_confirm_resume on the thread itself; the RELEASE/ACQUIRE pair publishes what the
        // waker wrote under the block lock.
        Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> switch_count = 0;

#if KICKOS_LIBC_REENT
        // This thread's struct _reent, seated by thread_create out of the app-side array.
        // switch_book stores it into libc's state word; nothing in the kernel dereferences it.
        // Here it is the second half of the pad before deadline_ns; anywhere below costs 8.
        void* reent = nullptr;
#endif

        uint64_t deadline_ns = 0;
        bool on_timer = false;

#if KICKOS_LIBC_REENT
        // TRUE WHILE `reent` STILL HOLDS WHATEVER THE PREVIOUS OCCUPANT LEFT. Cleared by the
        // first switch-in, which is what primes it. FREE HERE: on_timer's padding before `id`
        // absorbs it on every target, so thread_scalar_bytes is unmoved.
        bool reent_fresh = false;
#endif

        // Assigned in thread_create (KICKOS_TID_* above); 0 is idle-only after wrap.
        uint16_t id = 0;

        char name_buf[KICKOS_THREAD_NAME_MAX] = {};
        char const* name = nullptr; // -> name_buf (set in thread_create); never a user pointer
        uint8_t prio = 0;      // EFFECTIVE priority: the only field sched/policy/wq read.
                               // Sole writer is sched::set_prio (re-seats READY threads).
        uint8_t base_prio = 0; // assignment anchor; PI raises `prio` above it, never below
        Policy policy = Policy::FIFO;
        ThreadState state = ThreadState::INACTIVE;
        bool privileged = false;
        // Set once at the top of exit_current, never cleared: this thread is running its own
        // capability teardown. `state` cannot serve as the marker, the sweep releasing IrqLock
        // between chunks. Gates the cross-thread reply mint and the wake-during-teardown switch
        // deferral, which is PRIORITY-CONDITIONAL: sched::wake admits a strictly higher peer.
        bool dying = false;
        // CapAuthority (AUTH_*) bits. Read by cap_check_authority WITHOUT IrqLock, so it must
        // stay a single byte no path writes concurrently: the parent seats it at spawn before
        // the child runs, and only the thread itself narrows it. Ignored when `privileged`.
        // Fits the padding before quantum_ns; moving it grows every TCB.
        uint8_t authority = 0;
        // Count of CAP_IRQ entries in this thread's table; cap_teardown's pre-pass is the only
        // reader and releases nothing at zero. Takes the LAST padding byte before quantum_ns;
        // moving it grows every TCB.
        uint8_t cap_irq_live = 0;

        // Round-robin: quantum_ns == 0 means no slicing (pure FIFO within prio).
        uint32_t quantum_ns = 0;
        uint64_t slice_deadline_ns = 0;

        void* stack_base = nullptr;
        size_t stack_size = 0;
        // The dev window this thread holds, and the whole of the periph seam's possession gate.
        // Seated once at create from the spawn's grant; dev_size 0 means none, and a window at
        // base 0 is not expressible. This is AUTHORITY and not the mapping: a translating backend
        // maps task-wide (docs/design-m6-mmu.md F9).
        uintptr_t dev_base = 0;
        size_t dev_size = 0;
        // stack_base was demand-allocated by the kernel and must be harvested onto the free
        // list when this slot is reclaimed. A caller-owned stack is never harvested.
        bool kstack_owned = false;
        // Cancellation request (KOS_SYS_THREAD_KILL), a CancelKind. One-way: set by the
        // killer, never cleared, honoured at the target's own death point, its next syscall
        // ENTRY. A thread that never re-enters the kernel keeps running.
        uint8_t cancel_kind = CANCEL_NONE;
        // Who may cancel this thread: the KILL TAG of the thread that spawned it, or
        // KILL_TAG_NONE, which is 0 (ThreadPool is declared below this struct). It is the whole
        // of the kill gate and must never alias; see kill_tag_of and the clear in
        // ThreadPool::alloc.
        uint16_t spawner_tag = 0;
        // These three fit the padding before `task`; moving them grows every TCB.

        // The task this thread belongs to, owner of the memory domain the group shares. That
        // domain's regions are copied into `mpu` below at create, plus this thread's own private
        // regions, its stack and any DEV window it asked for. A pointer: an index beside it would
        // land past the saturated padding above and cost 8 bytes on every 32-bit TCB.
        Task* task = nullptr;
        MpuSet mpu;

        intptr_t wait_result = 0; // wake-status channel (mutex: 0 / -KOS_EOWNERDEAD;
                                  // endpoint: byte count >= 0, or -KOS_EPIPE); the waker
                                  // writes it before sched::wake, the sleeper reads it
                                  // after wq_block returns. Timed wait shares it.

        // Valid ONLY while this thread is parked on an endpoint waitq (send_waiters /
        // recv_waiters). The arriving peer reads it privileged under the lock to copy into or
        // out of this thread's buffer, which was bound-checked at park entry.
        struct IpcDesc
        {
            uintptr_t buf;       // this thread's own message buffer (already bound-checked)
            size_t    len;       // sender: bytes to send; receiver: buffer capacity
            uintptr_t badge_out; // receiver only: where to store the badge (0 => none)
        };
        IpcDesc ipc = {};

        // Valid while parked in a kos_call. call_rx_cap is the reply capacity in place; call_seq
        // is bumped per call and its low 8 bits ride the minted reply cap as the one-shot
        // late-reply ABA guard. Written by the caller before parking and by the popper/replier
        // under IrqLock, single-writer at every stage.
        size_t call_rx_cap = 0;
        uint16_t call_seq = 0;
        // A BITFIELD PAIR, and it has to stay one: the four bytes here are saturated against
        // `wait_obj`'s alignment, so a separate byte for the flag would cost every TCB four.
        // CallState spends two bits of the seven.
        uint8_t call_state : 7 = CALL_NONE;
        // Set ONLY by the trap-handler IPC fastpath, which parks a caller with no kernel
        // continuation: nothing reads wait_result on that thread's behalf, so the switch that
        // resumes it stores the result into the saved frame. Cleared there, by the one writer.
        uint8_t call_frame_parked : 1 = 0;
        WaitKind wait_kind = WAIT_NONE;

        // The object the wait edge names, valid only for the kinds that document one.
        // Read it through the accessors below and never raw: an untagged cast is how a
        // mis-tagged park reaches the wrong type.
        void* wait_obj = nullptr;
        // Head of the mutexes this thread OWNS, linked through Mutex::next_held;
        // thread_effective_prio scans it. Touched only under IrqLock at the mutex
        // block/unblock sites.
        Mutex* held_list = nullptr;

        // Callers parked in CALL_REPLY_WAIT on a reply cap THIS thread holds: one entry per live
        // CAP_REPLY in its table, linked through the caller's own `link`. The donor enumeration
        // for thread_effective_prio. The reply, close and teardown arms all empty it, so a dying
        // server never strands a donor.
        HeadList reply_waiters;

        // Per-THREAD capability table (cap.h). The run is reserved from the slab by the CALLER
        // before thread_create, so an exhausted slab fails the spawn instead of leaving a
        // half-built thread. thread_create's kmemset zeroes the directory and re-establishes it
        // from ThreadAttr afterwards. Every scan site must be bounded by thread_cap_capacity and
        // never by KICKOS_MAX_HANDLES: capacities differ per thread and 0 is legal.
        CapRun caps = {};
        // Head of the run's free-slot list (cap.h), a slot index biased by one. KCAP_FREE_NONE
        // is 0, so the thread_create kmemset leaves it EMPTY and not "slot 0": a thread whose
        // list was never threaded refuses every mint.
        uint16_t cap_free_head = KCAP_FREE_NONE;
#if KCAP_RUN_CHUNKS > 1
        // This thread's addressable capacity, 0 when it holds no run. Root's is
        // KICKOS_MAX_HANDLES and a spawned child's is narrower, so no reader may assume either.
        uint16_t cap_width = 0;
        // Live inbound CAP_REPLY entries, bounded by KICKOS_CAP_REPLY_MAX. Both fields land in
        // the tail padding the chunk directory's second pointer creates; keep the uint16_t group
        // CONTIGUOUS after `caps`.
        uint16_t cap_reply_live = 0;
#endif
        // Endpoints where ep->server == this thread, chained through Endpoint::next_served
        // (endpoint.h). The SEND_WAIT donor enumeration for thread_effective_prio, which runs
        // interrupt-masked and may walk neither the capability table nor the endpoint pool.
        // EP_SERVED_NONE is 0, so the thread_create kmemset leaves it empty.
        uint16_t served_head = EP_SERVED_NONE;

        // The one teardown writer for the edge wq_block and park_queueless establish. All
        // three go unconditionally: a partial clear leaves the tag and the queue disagreeing
        // about where this thread is parked. A caller's own unlink must run BEFORE this, since
        // the unlink reads wait_queue.
        void clear_wait_edge()
        {
            wait_queue = nullptr;
            wait_kind = WAIT_NONE;
            wait_obj = nullptr;
        }

        // The wait edge, narrowed to one kind each. TOTAL: every other kind answers
        // nullptr, which is what terminates the PI chain walk at a non-mutex park.
        Mutex* wait_mutex() const
        {
            if (wait_kind != WAIT_MUTEX)
            {
                return nullptr;
            }
            return static_cast<Mutex*>(wait_obj);
        }
        Endpoint* wait_endpoint() const
        {
            if (wait_kind != WAIT_EP_SEND and wait_kind != WAIT_EP_RECV)
            {
                return nullptr;
            }
            return static_cast<Endpoint*>(wait_obj);
        }
        Thread* wait_server() const
        {
            if (wait_kind != WAIT_EP_REPLY)
            {
                return nullptr;
            }
            return static_cast<Thread*>(wait_obj);
        }
        Thread* wait_join_target() const
        {
            if (wait_kind != WAIT_JOIN)
            {
                return nullptr;
            }
            return static_cast<Thread*>(wait_obj);
        }
        Task* wait_task_target() const
        {
            if (wait_kind != WAIT_TASK_EMPTY)
            {
                return nullptr;
            }
            return static_cast<Task*>(wait_obj);
        }
    };

    // The TCB budget; a deliberate TCB change lands by editing the scalar literal below.
    //
    // MEASURE ON A 32-BIT TARGET. A uint16_t added to Thread costs 8 bytes on armv6m and 0 on
    // the host: the padding before `task` is saturated on 32-bit and there is no tail padding.

    // ctx through switch_count, the word that saturates the padding before deadline_ns and the
    // last whose offset the target's context size can shift.
    constexpr size_t thread_head_bytes()
    {
        size_t members = sizeof(arch_context) + sizeof(ListNode) + sizeof(List*)
                         + sizeof(Thread*) + sizeof(uint32_t);
#if KICKOS_LIBC_REENT
        members = members + sizeof(void*); // Thread::reent
#endif
        return members + (alignof(uint64_t) - members % alignof(uint64_t)) % alignof(uint64_t);
    }

    // deadline_ns onwards, minus the MPU set and the capability directory. RXv3 aligns
    // uint64_t to 4 and so spends less padding here than every other 32-bit target.
    constexpr size_t thread_scalar_bytes()
    {
        size_t bytes = 116 - sizeof(size_t);
        if (sizeof(void*) == 8)
        {
            bytes = 180 - sizeof(size_t);
        }
        else if (alignof(uint64_t) == 4)
        {
            bytes = 112 - sizeof(size_t);
        }
#if KCAP_RUN_CHUNKS > 1
        bool const cap_pair_present = true;
#else
        bool const cap_pair_present = false;
#endif
        // cap_width + cap_reply_live. At 64 bits the pair lands in tail padding that exists
        // whether or not it does; a 32-bit target pays for it.
        if (sizeof(void*) == 8 or cap_pair_present)
        {
            bytes = bytes + 2 * sizeof(uint16_t);
        }
        // Thread::dev_base + Thread::dev_size, in the pointer-aligned run beside
        // stack_base/stack_size, so neither adds padding on any target.
        return bytes + sizeof(uintptr_t) + sizeof(size_t);
    }

    constexpr size_t KICKOS_THREAD_EXPECTED_SIZE =
        thread_head_bytes() + sizeof(MpuSet) + sizeof(CapRun) + thread_scalar_bytes();

    static_assert(sizeof(Thread) == KICKOS_THREAD_EXPECTED_SIZE,
                  "sizeof(Thread) moved. Either drop the member that grew the TCB, or re-measure "
                  "on a 32-BIT target and edit thread_scalar_bytes: a host measurement prices a "
                  "uint16_t at 0 and is blind to this");
    // The last member must close the struct up to its ALIGNMENT and no further; exact equality is
    // wrong, a 64-bit single-chunk build leaving four bytes of alignment tail. Anything past
    // alignof is a hole a reordered member opened.
    static_assert(offsetof(Thread, served_head) + sizeof(Thread::served_head)
                      > sizeof(Thread) - alignof(Thread),
                  "Thread grew tail padding beyond its alignment: the last member no longer "
                  "closes the struct, so the TCB now has slack a new member would land in for "
                  "free");
    // One CAP_IRQ entry occupies one slot, so a thread's count is bounded by its own table and
    // the widest table in the image is root's.
    static_assert(KICKOS_MAX_HANDLES <= UINT8_MAX,
                  "a thread could hold more CAP_IRQ entries than Thread::cap_irq_live can "
                  "count, and cap_teardown's pre-pass would skip a release it owes. The field "
                  "is a byte because that is what the TCB padding holds");

    // A thread's capability-table capacity: the width it was seated with if it holds a run,
    // else 0.
    inline uint32_t thread_cap_capacity(Thread const* t)
    {
#if KCAP_RUN_CHUNKS == 1
        // The flat run is one chunk of exactly KICKOS_MAX_HANDLES slots, so a held run IS the
        // ceiling and there is nothing to store.
        if (not cap_run_held(t->caps))
        {
            return 0;
        }
        return KICKOS_MAX_HANDLES;
#else
        return t->cap_width; // 0 == no run: attach and detach keep the two in step
#endif
    }

    // Return t's run to the slab and clear everything that travels with it: a stale width
    // answers thread_cap_capacity for a table that no longer exists, and a stale reply count
    // makes the slot's next occupant refuse its first caller. Caller holds IrqLock.
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

    struct ThreadAttr
    {
        char const* name = "thread";
        uint8_t prio = KICKOS_PRIO_MIN;
        Policy policy = Policy::FIFO;
        uint32_t quantum_ns = 0;
        // Default false: an attr struct that forgets the field must not mint privilege.
        bool privileged = false;
        // Optional domain data region granted to an unprivileged thread (RW).
        // Threads sharing one region share a memory domain; base==0 => none.
        void* mem_base = nullptr;
        size_t mem_size = 0;
        // Optional device/MMIO region granted to an unprivileged thread (R|W|DEV, never
        // executable). AUTH_MEMORY-only at the spawn boundary, and PER-THREAD: it lands in this
        // thread's own region set and never in its task's domain. base==0 => none.
        void* mmio_base = nullptr;
        size_t mmio_size = 0;
        // Pre-resolved task: thread_create_call sets it so a task- or domain-pool exhaustion fails
        // the spawn before anything is built. null => thread_create resolves from privileged +
        // mem_base, which only idle and root do.
        Task* task = nullptr;
        // Who is allowed to cancel the new thread (a kill tag). thread_create_call seats the
        // caller's; idle and root leave it NONE and are so un-killable. A handle DOES name
        // root, so this is the whole of its protection.
        uint16_t spawner_tag = 0;
        // The stack passed to thread_create was demand-allocated by the kernel and is owned by
        // the free list (harvest at reclaim). false for caller-owned and for the
        // arena-allocated idle/root stacks.
        bool kstack_owned = false;
        // Pre-reserved capability run (cap_slab_attach): an exhausted slab must fail the spawn
        // BEFORE anything is built. An empty directory is legal and means the thread holds no
        // capabilities. The free-list head and the seated capacity travel with it: a run seated
        // without its head refuses every mint.
        CapRun cap_run = {};
        uint16_t cap_free_head = KCAP_FREE_NONE;
        uint16_t cap_width = 0;
    };

    // Static thread-slot pool. Bump-allocated, then EXITED slots reclaimed at spawn. Liveness is
    // INTRINSIC: a slot is free iff its TCB state is EXITED. The per-slot generation bumps at
    // RECLAIM and not at exit, so a handle to a just-exited-but-not-yet-reused slot still
    // gen-matches and reuse invalidates it. Caller serializes (IrqLock).
    //
    // KICKOS_THREAD_SLOTS, not KICKOS_MAX_THREADS: kmain claims one slot for root before any
    // spawn can run, so a spawn still draws the full KICKOS_MAX_THREADS the board states.

#if KICKOS_KERNEL_STACKS
    // The per-slot kernel-stack instrumentation. `index` is a POOL index, 0 to
    // KICKOS_THREAD_SLOTS - 1; idle holds its TCB outside the pool and has no stack here.
    //
    // Per slot and since boot. The block is armed once at init and never re-armed when a slot
    // changes hands: re-arming would erase the record of an overflow that had already happened.
    void kstack_arm(int index);
    bool kstack_canary_intact(int index);
#endif

    struct ThreadPool
    {
        // The uint16_t generation takes the other 16, so the handle spends the whole word: a
        // fully aged one has bit 31 set and no sign test says anything about it. The kill tag
        // below, not this, caps the pool at 65534.
        static constexpr int INDEX_BITS = 16;
        // STRICTLY less: the all-ones index is reserved and never seated, which is what makes
        // KOS_THREAD_NONE unmintable by ANY generation and not merely out of the current
        // pool's range.
        static_assert(KICKOS_THREAD_SLOTS < (1 << INDEX_BITS),
                      "thread handle index field too small for KICKOS_THREAD_SLOTS, or the "
                      "pool would seat the index KOS_THREAD_NONE reserves");
        static_assert((KOS_THREAD_NONE & ((1u << INDEX_BITS) - 1u)) == ((1u << INDEX_BITS) - 1u),
                      "KOS_THREAD_NONE must carry the reserved all-ones index");

        // Kill-gate identity, DERIVED from the slot index. KILL_TAG_BOOT names idle and only
        // idle, the one TCB outside the pool: every other thread, root included, has a slot and
        // so a tag of its own.
        static constexpr uint16_t KILL_TAG_NONE = 0;
        static constexpr uint16_t KILL_TAG_BOOT = 0xFFFFu;
        static_assert(KICKOS_THREAD_SLOTS < KILL_TAG_BOOT,
                      "a pool slot's kill tag would collide with the boot tag");

        static constexpr uint16_t kill_tag_for_index(int index)
        {
            return static_cast<uint16_t>(index + 1);
        }

        Thread slots[KICKOS_THREAD_SLOTS];
        int next = 0;
        uint16_t gen[KICKOS_THREAD_SLOTS] = {};

        // A CAP_REPLY carries this handle in CapEntry::obj, whole and unmasked (cap.h). At
        // 16 + 16 the fit is EXACT: a widening would truncate the GENERATION from the top and
        // collapse cap_reply_caller's late-reply guard with nothing at runtime to report it.
        static_assert(INDEX_BITS + 8 * sizeof(gen[0]) <= 8 * sizeof(CapEntry::obj),
                      "a thread handle no longer fits CAP_REPLY's obj word");

        // Free list of reclaimed kernel-default stacks: a SINGLE size class
        // (KICKOS_USER_STACK_SIZE), so it cannot fragment and the link lives in the dead block
        // itself. A block enters the list only at the exited-slot reclaim point (alloc, below),
        // where its former owner is provably off-CPU.
        void* stack_free_list = nullptr;
        static_assert(KICKOS_USER_STACK_SIZE >= sizeof(void*),
                      "a reclaimed stack block must be able to hold the free-list link");
        // A kernel-default stack has no caller to refuse it: a spawn passing stack_base ==
        // nullptr takes the constant below unconditionally, so this is the only place the floor
        // can be enforced. It is the arch's measured red zone, not a preference.
        static_assert(KICKOS_USER_STACK_SIZE >= KICKOS_MIN_STACK_SIZE,
                      "KICKOS_USER_STACK_SIZE is below the arch's syscall stack floor: raise "
                      "it in the board defconfig, or the default spawn makes threads that "
                      "cannot make a syscall");

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

        // Claim a slot: reclaim an EXITED one (bumping its generation to kill stale handles) or
        // bump-allocate a fresh one. Returns the index, or -1 if full. Lowest-exited first, which
        // holds `next` down: the scan below and the spawner_tag sweep are both bounded by it.
        [[nodiscard]] int alloc()
        {
            for (int s = 0; s < next; s++)
            {
                if (slots[s].state == ThreadState::EXITED)
                {
                    // Harvest at the RECLAIM point: only by now is the thread provably off-CPU,
                    // so writing the free-list link into its stack cannot race the final context
                    // save. Ownership moves to the list, so a later reclaim never double-pushes.
                    if (slots[s].kstack_owned)
                    {
                        stack_push(slots[s].stack_base);
                        slots[s].kstack_owned = false;
                    }
                    // Same reclaim-point requirement as the stack: the thread is off-CPU and
                    // cap_teardown has emptied every entry. cap_slab_detach clears the directory
                    // and the free-list head as it returns each chunk.
                    thread_cap_release(&slots[s]);
                    // A slot's kill tag is INDEX-DERIVED and outlives its occupant: a child
                    // still naming this tag must be orphaned before the slot changes hands, or
                    // the new occupant inherits cancel authority over threads it never spawned.
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
            if (next >= KICKOS_THREAD_SLOTS)
            {
                return -1;
            }
            return next++;
        }

        // Undo a slot claimed by alloc() when the spawn fails AFTER the claim. Must NOT burn a
        // generation and must NOT leave a hole alloc() would never revisit, so it mirrors
        // alloc()'s two cases:
        //   * a reclaimed EXITED slot: revert alloc()'s generation bump, so the prior occupant's
        //     join-by-handle still resolves, and leave it EXITED and reclaimable.
        //   * a fresh bump slot, always the last one under the spawn lock: un-bump `next`, else
        //     it becomes a permanent hole.
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

        // Index of a TCB in this pool, or -1 if it is not a pool slot, which today is idle and
        // nothing else. Compares addresses as integers: subtracting pointers that may not point
        // into slots[] is UB.
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

        // Root's slot: kmain claims it before any spawn can run, so it is index 0 on every
        // board and every boot (kmain asserts it). Do NOT identify root by
        // spawner_tag == KILL_TAG_NONE instead: alloc's sweep above clears a reclaimed slot's
        // children to NONE, so that test names every orphan too.
        static constexpr int ROOT_INDEX = 0;

        bool is_root(Thread const* t) const
        {
            return t == &slots[ROOT_INDEX];
        }

        // The kill tag naming `t`: kill_tag_for_index of its slot if it is one of ours, else
        // the boot tag. Never KILL_TAG_NONE, so a thread with no spawner matches nobody.
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
