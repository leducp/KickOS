// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Tasks: the set of threads that share one memory domain. A task owns a Domain* and counts the
// live threads that joined it; the domain is refcounted by its live TASKS, so it returns to its
// pool when the last task holding it dies.
//
// Two ways in. A spawn is pthread_create: the child is a thread of the SPAWNER's task and spends
// no slot here. A program's create (task_create_declared) seats a declaration's instance, whose
// entry thread is its first member. The kernel's own idle and root take theirs through task_for.
//
// Three deaths end the group from a member's own exit: a FAULT, a task-scope exit (_exit), and the
// death by any cause of the task's ENTRY thread, the first member a non-member seats. The first of
// them gives the task its status, and an ended task takes no member. A cancel of any other member
// ends that member alone; a caller that wants the group calls task_cancel_group itself.
//
// A task owns a DEV window's LIFETIME and never its ACCESS: the window is a region of the asking
// THREAD, and the group cancel is what returns it.

#ifndef KICKOS_TASK_H
#define KICKOS_TASK_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h> // KICKOS_KERNEL_CORES, KICKOS_CORE_SET_ALL
#include <kickos/budget.h>
#include <kickos/config.h>
#include <kickos/held.h>
#include <kickos/list.h>
#include <kickos/pools.h>    // ThreadLink
#include <kickos/slotpool.h> // the handle codec

#include <kickos/sys/abi.h> // kos_task_t, KOS_TASK_NONE

namespace kickos
{
    struct Domain; // kickos/domain.h: the shared region set this task's threads see
    struct Thread; // kickos/thread.h: a member

    // A count of thread-pool slots, or a slot's kill tag: as wide as the thread-slot ceiling
    // needs.
    using TaskThreads = ThreadLink;

    // What links one task slot, in the pools' storage (<kickos/pools.h>): the next task its
    // creator holds, its index plus one, 0 for none. Kept across a declaration's incarnations:
    // the frame runs, the domains and the first RAM owner record it is charged for, the
    // declaration's record paying for each while it lives, and the thread slot, plus one, whose
    // system call holds a copy of the image's data staged for its next space. And the
    // capability runs its CAP_SLOTS budget is charged for, through Thread::reclaim.
    struct TaskLinks
    {
        HeadList cap_runs;
        uint8_t created_next;
#if KICKOS_HAVE_ASPACE
        HeadList runs;
        HeadList domains;
        ThreadLink image_stager;
#endif
#if not KICKOS_HAVE_ASPACE
        uint16_t ram;
#endif
#if KICKOS_KERNEL_CORES > 1
        HeadList irq_retiring;
#endif
    };

    struct Task
    {
        // Read from outside task.cc through the accessors below only. EVERY member must
        // stay ZERO-initialised: a non-zero initialiser anywhere in Kernel moves the whole
        // constinit instance out of .bss and into .data.
        Domain* domain = nullptr;
        // The task pool's free list (task.cc), and the threads naming this task, through their
        // Thread::member.
        ListNode free_node;
        HeadList members;
        TaskThreads refcount = 0; // live threads
        // The kill tag of the thread that created the task, or KILL_TAG_NONE for a kernel task.
        // Both the creator gate and the reservation: a slot with a creator is not free even at
        // refcount 0, which is what lets a created task exist before its first thread.
        TaskThreads creator_tag = 0;
        // Bumped when the slot is freed, so a handle to a dead task stops resolving.
        uint16_t gen = 0;
        // THE SCHEDULING GRANT. Checked once, where it is given, which is what lets a task
        // play freely with its own scheduling afterwards and consult no capability. Read
        // through the total accessors below and never raw: a stored 0 is the WHOLE grant, and
        // that is what keeps every member here zero-initialised.
        //
        // The ceiling is UNCONDITIONAL where the core set is not: without it any unprivileged
        // thread can take the top of the run queue, a starvation hole on a one-core board too.
        uint8_t prio_ceiling = 0;
        // TASK_MARK_* bits (task.cc): the armed endpoint has been waited on since the task last
        // emptied, which is what "ready" means for the creator's watch (kos_task_watch); the
        // task has ended and its status is set; it is dead, every member swept; it serves the
        // published console.
        uint8_t marks = 0;
        // The bit to raise, the arming capability's badge, as an index: one bit is all a
        // capability ever raises.
        uint8_t watch_bit = 0;
        // Members released and still sweeping their capabilities: the death is reported once
        // the task is empty and this is back to zero.
        TaskThreads sweeping = 0;
        // The image whose code and data its threads run (<kickos/image.h>).
        uint8_t image = 0;
#if KICKOS_KERNEL_CORES > 1
        uint32_t core_set = 0;
#endif
        // The notification a death or a first receive raises, by generational object handle
        // biased by one (notify_bound_store), 0 for none. Never held: freeing the notification
        // zeroes it.
        int32_t watch_notify = 0;
        // The endpoint whose first receive means ready, biased the same way; compared only.
        int32_t watch_ep = 0;
        // The status the task ended with, which kos_task_state answers once it has ended.
        int32_t exit_status = 0;
        // Its image's word the switch seats with the running thread's libc state, null where
        // libc asks for its state instead (reent_seat in <kickos/sys/image.h>).
        void* reent_seat = nullptr;
    };

    // What a task pool slot is priced at, on a 32-bit target at one kernel core: 44 bytes, and
    // one core-set word more above one core. MEASURE ON A 32-BIT TARGET if this fires: a host
    // build prices the tail differently, so a host measurement cannot settle it.
    constexpr size_t task_scalar_bytes()
    {
        size_t raw = sizeof(Domain*) + sizeof(ListNode) + sizeof(HeadList)
                     + sizeof(Task::refcount) + sizeof(Task::creator_tag)
                     + sizeof(Task::gen) + sizeof(Task::prio_ceiling) + sizeof(Task::marks)
                     + sizeof(Task::watch_bit) + sizeof(Task::sweeping)
                     + sizeof(Task::watch_notify)
                     + sizeof(Task::watch_ep) + sizeof(Task::exit_status);
        raw = raw + sizeof(Task::image) + sizeof(Task::reent_seat);
#if KICKOS_KERNEL_CORES > 1
        raw = raw + sizeof(Task::core_set);
#endif
        return ((raw + sizeof(void*) - 1) / sizeof(void*)) * sizeof(void*);
    }

    constexpr size_t KICKOS_TASK_EXPECTED_SIZE = task_scalar_bytes();

    static_assert(sizeof(Task) == KICKOS_TASK_EXPECTED_SIZE,
                  "sizeof(Task) moved. Either drop the member that grew it, or re-measure on a "
                  "32-BIT target and edit task_scalar_bytes: a host measurement prices this "
                  "differently from a 32-bit target");
    // No no-tail-padding assert to match Thread's: Domain*'s alignment leaves 4 bytes after
    // `gen` on the host and none on 32-bit, so such an assert cannot hold on both.

    // Handle codec. The index is stored BIASED BY ONE so that no live task is ever named by
    // the all-zero word: KOS_TASK_NONE is 0.
    constexpr int TASK_INDEX_BITS = HANDLE_INDEX_BITS;

    // Boot: clear the pool. Reads nothing from the domain pool, so it carries no ordering
    // constraint against domain_init.
    void task_init(void);

    // The domain this task's threads share, or null for a task holding nothing. Null-safe.
    Domain* task_domain(Task const* t);

    // Seat the kernel's own task of idle or root, its domain resolved through domain_for. Takes no
    // reference on either the task or the domain: task_ref, from thread_create, is the commit.
    //
    // `caller` is domain_for's posture word (DOM_CALLER_*), resolved by the caller and never read
    // from sched::current() here.
    //
    // Returns null on refusal and writes the reason to *err (never null; 0 on success). Every code
    // is domain_for's, forwarded unflattened, plus:
    //   KOS_ENOMEM  the task pool is full.
    Task* task_for(uint32_t caller, int* err);

    // Resolve a handle to a created task, or null. The kernel's own tasks are unnameable: idle's
    // carries the kernel domain, so a resolvable handle to it would let an unprivileged spawn
    // join the whole arena.
    Task* task_resolve(kos_task_t handle);
    kos_task_t task_handle(Task const* t);
    // The creator gate. False for a kernel task and for KILL_TAG_NONE, so a thread with
    // no spawner matches nobody, exactly as the thread kill gate answers.
    bool task_created_by(Task const* t, uint16_t tag);

    // Release the creator's hold: the group is no longer nameable, and the slot and its
    // domain go back as soon as the last member leaves (at once, if there is none). A member
    // its create has not started yet is started slain, so the task ends. Caller holds IrqLock.
    void task_drop_hold(Task* t, Held held);

    // The creator's reports (kos_task_watch). Arm or replace the watch on `task`, or disarm
    // it with KOS_CAP_NONE for `notify_cap`: 0, -KOS_EBADF (no such task, or a cap naming
    // nothing of its kind), -KOS_EPERM (not the creator), -KOS_EACCES (the notification cap
    // lacks SIGNAL).
    int task_watch_call(kos_task_t task, uint32_t notify_cap, uint32_t ready_ep);

    // The image whose code and data `t`'s threads run (<kickos/image.h>): its declaration's for
    // a declared instance, and image 0, the init's, for a task the boot seats. Null-safe: image 0.
    size_t task_image(Task const* t);

    // Declaration `decl`'s own task record, which every incarnation of its instance reuses:
    // 0 when a create may seat it, after ending a dead incarnation's creator hold, whose handle
    // then answers -KOS_EBADF; -KOS_EBUSY while an incarnation runs or another thread's create
    // holds a copy of its data staged, -KOS_EAGAIN once it ended
    // and its members are still dying, -KOS_EPERM once one started of a declaration with neither
    // KOS_DECL_RESTARTS nor KOS_DECL_ON_REQUEST. Caller holds IrqLock.
    int task_decl_state(size_t decl);

    // Seats declaration `decl`'s instance in its own record, held by `creator_tag`, running
    // image `image`: domain_for's refusals, a free record being task_decl_state's 0. Caller
    // holds IrqLock.
    Task* task_create_declared(size_t decl, size_t image, uint16_t creator_tag, void* mem_base,
                               size_t mem_size, uint32_t mem_attr, Domain* donor, int* err,
                               Held held);
    // `t`, a declaration's record task_create_declared seated, has started its entry thread.
    void task_mark_started(Task* t);
    // The endpoint a member's first receive on makes `t` ready, from its create on, whether or
    // not a watch is armed yet. Caller holds IrqLock.
    void task_ready_seat(Task* t, int ep_obj);
    // The notification `notify_handle` names is freed: no watch raises it from here on, so a slot
    // seated again is never raised into. Caller holds IrqLock.
    void task_watch_notify_gone(int notify_handle);
    // KOS_TASK_LIVE | READY | DEAD | ENDED for the task `task` names, with *status the status
    // it ended with where ENDED is set, or -KOS_EBADF / -KOS_EPERM.
    int task_state_call(kos_task_t task, int32_t* status);
    // The task ends, its entry thread having died by any cause, or a member faulted or exited
    // with the task scope: it takes no member from here on, the creator's watch is raised, and
    // `code` is its status when `latch` holds, the ending thread being uncancelled; otherwise it
    // keeps KOS_EXIT_CANCELLED. A later end raises and latches nothing. Every other member is
    // slain (CANCEL_SLAY), with or without a creator.
    void task_end(Task* t, int code, bool latch, Held held);
    // Kill and slay: `t` ends now, before its members have run to their deaths, and every member
    // is stopped.
    void task_stop(Task* t, Held held);
    // Whether `t` has ended, which refuses it a new member. Null-safe: false.
    bool task_ended(Task const* t);
    // `t` serves the published console from here on, and no other task does: its end, or its
    // slot going back, is the console's death (cap_console_task_ended). Caller holds IrqLock.
    void task_console_serve(Task* t);
    // Whether `t` serves the published console, its task not yet ended. Null-safe: false.
    bool task_serves_console(Task const* t);
    // A member released from `t`, of generation `gen`, has swept its capabilities: true when
    // that leaves the task of generation `gen` empty with no member sweeping, which is its
    // death and what a WAIT_TASK_EMPTY waiter waits for. Caller holds IrqLock.
    bool task_sweep_done(Task* t, uint16_t gen);
    // The task emptied and every member's sweep ran: mark it ended and dead and raise the
    // watch, once, if `t` is still the task of generation `gen`. Caller holds IrqLock.
    void task_report_death(Task* t, uint16_t gen);
    uint16_t task_gen(Task const* t); // null-safe: 0
    // A member of `t` is waiting to receive on the endpoint `ep_obj` names: raise the watch
    // the first time that is the armed endpoint, without a reschedule, since the receive has
    // not parked yet. Answers the creator woken to run first, for the receive's deferred wake,
    // or null. Caller holds IrqLock.
    Thread* task_note_receive(Task* t, int ep_obj);

    // Drop the hold of EVERY task a dying creator holds. Keyed on the tag, which is the whole
    // gate: kill_tag_for_index derives it from the pool slot, so a recycled slot would inherit
    // creator authority over groups it never made. O(the tasks the tag's slot holds), from the
    // slot's created list.
    void task_orphan_created_by(uint16_t tag, Held held);

    // The declaration whose own record `t` is, or -1 for a record the boot seats. Null-safe.
    int task_decl_of(Task const* t);
#if KICKOS_HAVE_ASPACE
    // The list of frame runs `t`'s record pays for, which outlives the record's incarnations;
    // null for a task outside the pool.
    HeadList* task_runs_head(Task const* t);
    // The list of domains `t`'s record pays for, as task_runs_head.
    HeadList* task_domains_head(Task const* t);
    // The thread slot, plus one, holding a staged copy of the data of `t`'s record's next space
    // (TaskLinks::image_stager); null for a task outside the pool.
    ThreadLink* task_image_stager(Task const* t);
    // Declaration `decl`'s own record, or null past the declarations.
    Task* task_decl_record(size_t decl);
#endif
#if KICKOS_KERNEL_CORES > 1
    // The IRQ bindings `t`'s record pays for between their last capability's release and their
    // reclamation; null for a task outside the pool.
    HeadList* task_irq_retiring(Task const* t);
    // A binding left `t`'s list of retiring bindings: a record that held nothing else is free
    // again. Caller holds IrqLock.
    void task_irq_retired(Task* t);
#endif
#if not KICKOS_HAVE_ASPACE
    // The head of the chain of RAM owner records `t`'s record pays for, as task_runs_head.
    uint16_t* task_ram_head(Task const* t);
    // RAM_BYTES or RAM_BLOCKS of every record on that chain: every incarnation's, as nothing
    // frees a record. O(the record's blocks). Caller holds IrqLock.
    uint32_t task_ram_usage(Task const* t, BudgetKind kind);
#endif

    // The scheduling grant, TOTAL over a zero-initialised or absent task: a stored 0 is the
    // whole grant, so idle and root answer the machine's full width without an
    // initialiser. Null-safe.
    uint8_t task_prio_ceiling(Task const* t);
#if KICKOS_KERNEL_CORES > 1
    uint32_t task_core_set(Task const* t);

    // The cores a member takes when it asks for none: the grant less the isolated ones, which
    // is what keeps automatic placement off them. A grant naming nothing else answers itself,
    // so this is never empty where the grant is not.
    uint32_t task_default_cores(Task const* t);
#endif

    // Narrow `t`'s grant. `ceiling` 0 and `cores` 0 each mean "leave that half alone", which is
    // what a caller naming only one of them passes. Returns 0, -KOS_EPERM for a request wider
    // than the grant it is narrowing, which is refused and never clamped, or -KOS_EINVAL for
    // one naming no core this kernel schedules.
    int task_sched_narrow(Task* t, uint8_t ceiling, uint32_t cores);

    // A new task takes its creator's grant WHOLE, both halves together; task_sched_narrow is
    // what moves it afterwards. A task that defaulted to the full machine would be a way to
    // widen a grant by creating a group.
    inline void task_sched_inherit(Task* child, Task const* from)
    {
        child->prio_ceiling = task_prio_ceiling(from);
#if KICKOS_KERNEL_CORES > 1
        child->core_set = task_core_set(from);
#endif
    }

    // Whether two threads belong to one task. THE tree's answer to that question. A task is one
    // SCHEDULING DOMAIN: its threads share a single grant, so a placement gated on this predicate
    // stays inside that grant. False when either side holds no task.
    bool task_same_group(Thread const* a, Thread const* b);

    // Live members. Null-safe (0).
    TaskThreads task_member_count(Task const* t);
    // Released members whose capability sweep has not finished. Null-safe (0).
    TaskThreads task_sweeping(Task const* t);

    // `th`, pool slot `slot` (-1 for a TCB outside the pool, which no list holds), names `t` as
    // its task and joins its member list. Caller holds IrqLock. O(1).
    void task_member_join(Task* t, Thread* th, int slot);
    // `th`, pool slot `slot`, names no task and leaves its member list; nothing for a thread
    // naming none. Caller holds IrqLock. O(1).
    void task_member_leave(Thread* th, int slot);
    // Every thread naming `t` as its task, released members still sweeping included: the first
    // one's Thread::member, nullptr for none, each node's `next` giving the next.
    ListNode* task_members(Task const* t);

    // Thread-pool slot `slot`'s capability run joins the run list of `t`, whose CAP_SLOTS budget
    // it is charged to until task_run_leave. Null-safe. Caller holds IrqLock. O(1).
    void task_run_join(Task* t, int slot);
    // Slot `slot`'s run leaves `t`'s run list; nothing for a run on none. Caller holds IrqLock.
    // O(1).
    void task_run_leave(Task* t, int slot);

    void task_ref(Task* t); // a thread joins; the first one acquires the domain
    // A thread leaves, and counts as sweeping until its task_sweep_done. A created task outlives
    // its members until its creator drops the hold, and the last one out then releases the domain
    // and frees the slot, which free_slot seats again only once every sweep under it is done. TRUE when THIS call emptied the group; the transition happens
    // once and only its cause can see it.
    bool task_release(Task* t, Held held);

    // Slay every live member: each is marked and woken out of whatever it is parked on, and its
    // next resume is claimed, so it reaches its own death point. The group dies by ONE rule:
    // there is no exception argument, and thread_cancel_kind already refuses a dying thread.
    void task_cancel_group(Task* t, Held held);
}

#endif
