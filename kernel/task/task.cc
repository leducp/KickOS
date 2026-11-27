// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/task.h>

#include <kickos/debug.h> // KICKOS_DEBUG_ASSERT
#include <kickos/aspace.h>
#include <kickos/budget.h>
#include <kickos/cap.h>
#include <kickos/domain.h>
#include <kickos/endpoint.h>
#include <kickos/image.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
#include <kickos/ramown.h>
#include <kickos/sched.h>
#include <kickos/start_watch.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include <stdint.h> // UINT8_MAX

namespace kickos
{
    namespace
    {
        // The first three are their KOS_TASK_* bits, so task_state_call answers them as stored.
        constexpr uint8_t TASK_MARK_READY = KOS_TASK_READY;
        constexpr uint8_t TASK_MARK_DEAD = KOS_TASK_DEAD;
        constexpr uint8_t TASK_MARK_ENDED = KOS_TASK_ENDED;
        constexpr uint8_t TASK_MARK_CONSOLE = 1u << 4; // serves the published console
        // Slain with members left: the creator's hold goes at the death's report.
        constexpr uint8_t TASK_MARK_SLAIN = 1u << 5;
        // Its threads are owed a recompute of what the watches on its death lend them
        // (pi_drain_deaths).
        constexpr uint8_t TASK_MARK_WATCH_OWED = 1u << 6;
        // A declaration's record: an incarnation of it has been started. Kept across its
        // successors.
        constexpr uint8_t TASK_MARK_STARTED = 1u << 7;

        TaskLinks* task_links()
        {
            return pool_local(kickos_kernel_pools.task_links);
        }

        // The pool index of `t`, or -1 for a task outside the pool.
        int slot_of(Task const* t)
        {
            PoolSpan<Task> const tasks = kernel().tasks();
            uintptr_t const at = reinterpret_cast<uintptr_t>(t);
            uintptr_t const lo = reinterpret_cast<uintptr_t>(tasks.base);
            if (at < lo or at >= reinterpret_cast<uintptr_t>(tasks.base + tasks.size()))
            {
                return -1;
            }
            return static_cast<int>(t - tasks.base);
        }

        // Whether a released IRQ binding is still charged to `t`'s record, until its reclamation.
        bool retiring(Task const& t)
        {
#if KICKOS_KERNEL_CORES > 1
            int const i = slot_of(&t);
            return i >= 0 and task_links()[i].irq_retiring.head != nullptr;
#else
            (void)t;
            return false;
#endif
        }

        // A slot is free iff it holds no live thread, nobody reserved it, and no released
        // member still sweeps under its name, nor a released IRQ binding retires at its charge:
        // either counts in the budget of whichever task holds the slot. There are no immortal
        // tasks: even idle and root each get one, so nothing here needs domain.cc's immortal arm.
        bool slot_free(Task const& t)
        {
            return t.refcount == 0 and t.creator_tag == ThreadPool::KILL_TAG_NONE
                   and t.sweeping == 0 and not retiring(t);
        }

        // The slots past the declarations' own, which every other task takes.
        int shared_slots()
        {
            return kernel().tasks().size() - static_cast<int>(image_decl_count());
        }

        // Declaration `decl`'s own record, from the top of the pool down.
        Task& decl_slot(size_t decl)
        {
            PoolSpan<Task> const tasks = kernel().tasks();
            return tasks[tasks.size() - 1 - static_cast<int>(decl)];
        }

        // A shared slot that has become free heads the free list, once. O(1).
        void free_put(Task* t)
        {
            Kernel& k = kernel();
            int const i = slot_of(t);
            if (i < 0 or i >= shared_slots() or not slot_free(*t)
                or k.task_free.holds(&t->free_node))
            {
                return;
            }
            k.task_free.push(&t->free_node);
        }

        // The slot freed last, else a never-seated one. Amortized O(1): the never-seated cursor
        // only advances.
        Task* free_slot()
        {
            Kernel& k = kernel();
            ListNode* const freed = k.task_free.pop();
            if (freed != nullptr)
            {
                return KICKOS_CONTAINER_OF(freed, Task, free_node);
            }
            int const n = shared_slots();
            while (k.task_fresh < n and not slot_free(k.tasks()[k.task_fresh]))
            {
                k.task_fresh++;
            }
            if (k.task_fresh >= n)
            {
                return nullptr;
            }
            k.task_fresh++;
            return &k.tasks()[k.task_fresh - 1];
        }

        // Slot `t`, free, seated for `d`, which domain_for built, its threads running image
        // `image`.
        void task_fill(Task* t, Domain* d, size_t image)
        {
            uint16_t const gen = t->gen;
            *t = Task{};
            t->gen = gen;
            t->domain = d;
            t->image = static_cast<uint8_t>(image);
#if KICKOS_LIBC_REENT and not KICKOS_REENT_PER_THREAD
            t->reent_seat = reinterpret_cast<void*>(image_record(image).reent_seat);
#endif
            // A task whose members are all cancelled ends with this status.
            t->exit_status = KOS_EXIT_CANCELLED;
        }

        // A slot for `d`, running image `image`. On a full pool `d` is released: a domain built
        // by a handoff holds a reference on its DONOR, and an abandoned one pins that donor's
        // space and every frame in it until the slot happens to be reused.
        Task* task_seat(Domain* d, size_t image, int* err)
        {
            Task* const t = free_slot();
            if (t == nullptr)
            {
                domain_release(d);
                *err = KOS_ENOMEM; // pool exhausted: retry later
                return nullptr;
            }
            task_fill(t, d, image);
#if KICKOS_HAVE_ASPACE
            domain_pay(d, t);
#endif
            return t;
        }

        // Task::watch_bit: the bit index below, the events asked above it, each KOS_TASK_* bit
        // shifted.
        constexpr uint8_t WATCH_INDEX_MASK = 0x1Fu;
        constexpr unsigned WATCH_EVENT_SHIFT = 4;
        static_assert(((KOS_TASK_READY | KOS_TASK_DEAD | KOS_TASK_ENDED) << WATCH_EVENT_SHIFT)
                              <= UINT8_MAX
                          and (KOS_TASK_READY << WATCH_EVENT_SHIFT) > WATCH_INDEX_MASK,
                      "Task::watch_bit holds a bit index and the three TASK events");

        // The slot goes back: no watch, no ready endpoint. Caller holds IrqLock.
        void watch_clear(Task* t)
        {
            t->watch_notify = 0;
            t->watch_ep = 0;
            t->watch_bit = 0;
            t->marks = static_cast<uint8_t>(t->marks & ~TASK_MARK_READY);
        }

        uint32_t watch_mask(Task const* t)
        {
            return 1u << (t->watch_bit & WATCH_INDEX_MASK);
        }

        bool watch_asks(Task const* t, uint8_t marks)
        {
            return ((static_cast<uint32_t>(t->watch_bit) >> WATCH_EVENT_SHIFT) & marks) != 0;
        }

        // The watch's notification, which task_watch_notify_gone keeps from naming a slot
        // seated again.
        Notification* watch_target(Task const* t)
        {
            if (t->watch_notify == 0)
            {
                return nullptr;
            }
            return kernel().notifies.resolve(notify_bound_handle(t->watch_notify));
        }

        // Raise the watch where it asks for any of `marks`, the events just reported.
        void watch_raise(Task const* t, uint8_t marks)
        {
            if (not watch_asks(t, marks))
            {
                return;
            }
            Notification* const n = watch_target(t);
            if (n != nullptr)
            {
                (void)notify_raise(n, watch_mask(t));
            }
        }

        // Whether `t`'s watch raises at its death, it having ended and not died yet: a waiter on
        // that bit lends to its threads.
        bool watch_lends(Task const* t)
        {
            return task_ended(t) and (t->marks & TASK_MARK_DEAD) == 0
                   and watch_asks(t, TASK_MARK_DEAD) and t->watch_notify != 0;
        }

        // `t` ended: each watch on its death marks its notification, so a wait parking or woken
        // there recomputes `t`'s threads, and a waiter already parked lends now.
        void lend_on_end(Task* t, Held held)
        {
            bool lends = false;
            if (watch_lends(t))
            {
                Notification* const n = watch_target(t);
                if (n != nullptr)
                {
                    notify_lend_mark(n);
                    lends = true;
                }
            }
            int const decl = task_decl_of(t);
            if (decl >= 0 and start_watch_mark_decl(static_cast<size_t>(decl)))
            {
                lends = true;
            }
            if (lends)
            {
                watch_lend_task(t, held);
            }
        }

        // The task no longer serves the console: it ended, or its slot went back without it ever
        // ending.
        void console_leave(Task* t, Held held)
        {
            if ((t->marks & TASK_MARK_CONSOLE) == 0)
            {
                return;
            }
            t->marks = static_cast<uint8_t>(t->marks & ~TASK_MARK_CONSOLE);
            cap_console_task_ended(held);
        }

        // A caller that holds no member re-reads `t`'s generation after it: a switch inside can
        // let `t` die and its slot be seated again.
        void mark_ended(Task* t, int code, bool latch, Held held)
        {
            if ((t->marks & TASK_MARK_ENDED) != 0)
            {
                return;
            }
            uint16_t const gen = t->gen;
            t->marks = static_cast<uint8_t>(t->marks | TASK_MARK_ENDED);
#if KICKOS_HAVE_ASPACE
            domain_retire_handles(t->domain);
#endif
            if (latch)
            {
                t->exit_status = code;
            }
            console_leave(t, held);
            if (t->gen != gen)
            {
                return;
            }
            // Ahead of the raise, whose wake can switch this thread out.
            lend_on_end(t, held);
            watch_raise(t, TASK_MARK_ENDED);
        }

        // The slot and its domain go back, and the generation bump is what stops a handle
        // still naming this task from resolving onto its successor.
        void free_task(Task* t, Held held)
        {
            console_leave(t, held);
            // A member still sweeping keeps the slot from being seated again, and the watch for
            // the DEAD task_report_death raises at the last sweep, its lend with it.
            if (t->sweeping == 0)
            {
                task_watch_disarm(t, held);
                watch_clear(t);
            }
#if KICKOS_HAVE_ASPACE
            if ((t->marks & TASK_MARK_ENDED) == 0)
            {
                domain_retire_handles(t->domain);
            }
            domain_retire_space(t->domain);
            // What a borrower keeps of the space stays charged to this record until it goes.
            Domain* const donor = domain_retire_handoff(t->domain);
            domain_shed(t->domain);
#endif
            domain_release(t->domain);
#if KICKOS_HAVE_ASPACE
            // Released here, not inside the retire: each release is a chain of its own.
            domain_release(donor);
#endif
            // Nulling makes the debris FAIL-CLOSED: task_domain answers null, which every
            // reader already handles, rather than a Domain* the pool may have re-handed.
            t->domain = nullptr;
            t->creator_tag = ThreadPool::KILL_TAG_NONE;
            t->gen++;
            free_put(t);
        }
    }

    static_assert(KICKOS_THREAD_SLOT_CEILING < (1ull << (8 * sizeof(TaskThreads))) - 1u,
                  "Task::refcount and Task::sweeping must fit every TCB live at once, and a pool "
                  "slot's kill tag would alias another, or idle's boot tag, once truncated into "
                  "Task::creator_tag");

    void task_init(void)
    {
        Kernel& k = kernel();
        TaskLinks* const l = task_links();
        for (int i = 0; i < k.tasks().size(); i++)
        {
            k.tasks()[i] = Task{};
            l[i] = TaskLinks{};
        }
        k.task_holds = 0;
        k.task_free = HeadList{};
        k.task_fresh = 0;
        // Each core's idle task and root's, beside every declaration's own.
        if (shared_slots() < KICKOS_KERNEL_CORES + 1)
        {
            kpanic("task pool: the pool holds no record for idle and root beside the "
                   "declared tasks");
        }
    }

    Domain* task_domain(Task const* t)
    {
        if (t == nullptr)
        {
            return nullptr;
        }
        return t->domain;
    }

    Task* task_for(uint32_t caller, int* err)
    {
        // Admission must run before a task slot is spent, or a refused domain leaves debris.
        Domain* const d = domain_for(caller, nullptr, 0, 0u, nullptr, err);
        if (d == nullptr)
        {
            return nullptr;
        }
        return task_seat(d, 0, err);
    }

    namespace
    {
        // The creator's hold on `t`, taken before the reservation is visible: a created task
        // sits at refcount 0 between create and its first spawn, and only this reference stops
        // the domain pool re-handing the slot underneath it. `t` heads its creator slot's
        // created list.
        void task_hold(Task* t, Domain* d, uint16_t creator_tag)
        {
            domain_ref(d);
            t->creator_tag = static_cast<TaskThreads>(creator_tag);
            Kernel& k = kernel();
            int const ti = slot_of(t);
            if (ti >= 0 and creator_tag != ThreadPool::KILL_TAG_NONE
                and creator_tag <= k.threads.count())
            {
                ThreadLinks& creator = k.threads.links()[creator_tag - 1];
                task_links()[ti].created_next = creator.created_head;
                creator.created_head = static_cast<uint8_t>(ti + 1);
            }
            k.task_holds++;
#if KICKOS_ARCH_ARENA_DCACHE and not KICKOS_HAVE_ASPACE
            // Not in domain_for, whose spawn chain is the deeper one and whose grant is always
            // Normal.
            if (domain_region_count(d) != 0)
            {
                arch_mpu_region const* const r = domain_region_at(d, 0);
                grant_sync(nullptr, r->base, r->size, r->attr);
            }
#endif
        }
    }

    Task* task_resolve(kos_task_t handle)
    {
        // Unbiased, so KOS_TASK_NONE and every word carrying no index at all wrap out of range.
        uint32_t const index = handle_index(handle) - 1u;
        if (index >= static_cast<uint32_t>(kernel().tasks().size()))
        {
            return nullptr;
        }
        Task* const t = &kernel().tasks()[index];
        if (t->gen != handle_gen(handle))
        {
            return nullptr;
        }
        if (t->creator_tag == ThreadPool::KILL_TAG_NONE)
        {
            return nullptr; // a kernel task is unnameable, and a freed slot has no creator
        }
        return t;
    }

    kos_task_t task_handle(Task const* t)
    {
        PoolSpan<Task> const tasks = kernel().tasks();
        int const index = slot_index_of<Task>(tasks.base, tasks.size(), t);
        if (index < 0)
        {
            return KOS_TASK_NONE;
        }
        return handle_pack(t->gen, static_cast<uint32_t>(index) + 1u);
    }

    bool task_created_by(Task const* t, uint16_t tag)
    {
        if (t == nullptr or tag == ThreadPool::KILL_TAG_NONE)
        {
            return false;
        }
        return t->creator_tag == static_cast<TaskThreads>(tag);
    }

    Task* task_resolve_created(Thread* c, kos_task_t task, int* err)
    {
        Task* const t = task_resolve(task);
        if (t == nullptr)
        {
            *err = KOS_EBADF;
            return nullptr;
        }
        if (not task_created_by(t, kernel().threads.kill_tag_of(c)))
        {
            *err = KOS_EPERM;
            return nullptr;
        }
        return t;
    }

    bool task_watch_ready_named(Task const* t)
    {
        return t->watch_ep != 0;
    }

    void task_watch_disarm(Task* t, Held held)
    {
        bool const lent = watch_lends(t);
        t->watch_notify = 0;
        t->watch_bit = 0;
        if (lent)
        {
            watch_lend_task(t, held);
        }
    }

    void task_watch_arm(Task* t, int notify_obj, uint32_t bit, uint32_t events, Held held)
    {
        bool lent = watch_lends(t);
        // No reference: whoever can receive the report holds a capability or a binding, and
        // either keeps the object alive; a report nobody can receive has nowhere to go.
        t->watch_notify = notify_bound_store(notify_obj);
        t->watch_bit = static_cast<uint8_t>((bit & WATCH_INDEX_MASK) | (events << WATCH_EVENT_SHIFT));
        Notification* const n = watch_target(t);
        if (n != nullptr and watch_lends(t))
        {
            notify_lend_mark(n);
            lent = true;
        }
        if (lent)
        {
            watch_lend_task(t, held);
        }
        watch_raise(t, static_cast<uint8_t>(t->marks & (TASK_MARK_READY | TASK_MARK_DEAD
                                                        | TASK_MARK_ENDED)));
    }

    uint8_t task_watch_lend(Task const* t, uint8_t p)
    {
        if (not watch_lends(t))
        {
            return p;
        }
        Notification const* const n = watch_target(t);
        if (n == nullptr)
        {
            return p;
        }
        uint8_t const w = notify_lender_prio(n, watch_mask(t));
        if (w > p)
        {
            return w;
        }
        return p;
    }

    bool task_watch_owe_into(int notify_obj)
    {
        int32_t const stored = notify_bound_store(notify_obj);
        bool any = false;
        for (Task& t : kernel().tasks())
        {
            if (t.watch_notify == stored and watch_lends(&t))
            {
                task_watch_lend_owe(&t);
                any = true;
            }
        }
        return any;
    }

    int task_state_call(kos_task_t task, int32_t* status)
    {
        IrqLock lock;
        int err = KOS_EBADF;
        Task* t = nullptr;
        if (task == KOS_TASK_NONE)
        {
            t = sched::current()->task;
        }
        else
        {
            t = task_resolve_created(sched::current(), task, &err);
        }
        if (t == nullptr)
        {
            return -err;
        }
        int state = t->marks & (TASK_MARK_READY | TASK_MARK_DEAD | TASK_MARK_ENDED);
        if (t->refcount != 0)
        {
            state |= KOS_TASK_LIVE;
        }
        // Only the caller's own task can lack a creator here: a handle resolves to a held task.
        if (t->creator_tag == ThreadPool::KILL_TAG_NONE)
        {
            state |= KOS_TASK_ORPHANED;
        }
        if ((state & KOS_TASK_ENDED) != 0)
        {
            *status = t->exit_status;
        }
        return state;
    }

    void task_end(Task* t, int code, bool latch, Held held)
    {
        if (t == nullptr)
        {
            return;
        }
        // The ending thread is still a member, so a count of one leaves nobody to cancel.
        bool const others = t->refcount > 1;
        // Marked before mark_ended, whose wakes can switch this thread out.
        if (others)
        {
            task_cancel_mark(t, held);
        }
        mark_ended(t, code, latch, held);
        if (others)
        {
            task_cancel_parks(t, held);
        }
    }

    namespace
    {
        // False when a switch inside let `t` die and its slot go back, maybe seated again.
        // Inlined: task_stop's only kernel caller is the slay, so an image links this copy alone.
        inline __attribute__((always_inline)) bool stop_group(Task* t, Held held)
        {
            uint16_t const gen = t->gen;
            // Marked before mark_ended, whose wakes can switch a running caller out.
            task_cancel_mark(t, held);
            mark_ended(t, 0, /*latch=*/false, held);
            if (t->gen != gen)
            {
                return false;
            }
            task_cancel_parks(t, held);
            return t->gen == gen;
        }
    }

    void task_stop(Task* t, Held held)
    {
        if (t == nullptr)
        {
            return;
        }
        (void)stop_group(t, held);
    }

    void task_watch_notify_gone(int notify_handle)
    {
        int32_t const stored = notify_bound_store(notify_handle);
        for (Task& t : kernel().tasks())
        {
            if (t.watch_notify == stored)
            {
                t.watch_notify = 0;
            }
        }
    }

    void task_console_serve(Task* t)
    {
        for (Task& each : kernel().tasks())
        {
            each.marks = static_cast<uint8_t>(each.marks & ~TASK_MARK_CONSOLE);
        }
        if (t != nullptr)
        {
            t->marks = static_cast<uint8_t>(t->marks | TASK_MARK_CONSOLE);
        }
    }

    bool task_serves_console(Task const* t)
    {
        return t != nullptr and (t->marks & TASK_MARK_CONSOLE) != 0;
    }

    void task_watch_lend_owe(Task* t)
    {
        if (t == nullptr)
        {
            return;
        }
        t->marks = static_cast<uint8_t>(t->marks | TASK_MARK_WATCH_OWED);
        kernel().watch_lend_owed = true;
    }

    bool task_watch_lend_owed_take(Task* t)
    {
        if (t == nullptr or (t->marks & TASK_MARK_WATCH_OWED) == 0)
        {
            return false;
        }
        t->marks = static_cast<uint8_t>(t->marks & ~TASK_MARK_WATCH_OWED);
        return true;
    }

    Task* task_run_owner(Thread const* th)
    {
        ListNode const* const first = task_run_first(th);
        TaskLinks const* const l = task_links();
        Kernel& k = kernel();
        for (int i = 0; i < k.tasks().size(); i++)
        {
            if (l[i].cap_runs.head == first)
            {
                return &k.tasks()[i];
            }
        }
        return nullptr;
    }

    namespace
    {
        // Each member its create has not started yet is started slain.
        void start_unstarted_slain(Task* t, Held held)
        {
            // Added first: a thread not yet added takes no cancel.
            for (ListNode* m = t->members.head; m != nullptr; m = m->next)
            {
                Thread* const th = KICKOS_CONTAINER_OF(m, Thread, member);
                if (th->state == ThreadState::INACTIVE)
                {
                    sched::add(th, held);
                    thread_cancel_kind(th, CANCEL_SLAY, held);
                }
            }
        }

        // True when a member still holds the slot. Inlined: a frame here deepens the exit stubs'
        // teardown chain that the trap stack reservations bound.
        inline __attribute__((always_inline)) bool release_hold(Task* t, Held held)
        {
            if (t == nullptr or t->creator_tag == ThreadPool::KILL_TAG_NONE)
            {
                return false;
            }
            start_unstarted_slain(t, held);
            Kernel& kn = kernel();
            int const ti = slot_of(t);
            uint16_t const tag = t->creator_tag;
            if (ti >= 0 and tag <= kn.threads.count())
            {
                // Out of the creator slot's created list. O(the tasks that slot holds).
                TaskLinks* const tl = task_links();
                uint8_t* at = &kn.threads.links()[tag - 1].created_head;
                while (*at != 0 and *at != static_cast<uint8_t>(ti + 1))
                {
                    at = &tl[*at - 1].created_next;
                }
                if (*at != 0)
                {
                    *at = tl[ti].created_next;
                }
                tl[ti].created_next = 0;
            }
            t->creator_tag = ThreadPool::KILL_TAG_NONE;
            t->marks = static_cast<uint8_t>(t->marks & ~TASK_MARK_SLAIN);
            // The only place a live creator tag is cleared, which keeps task_holds exact:
            // free_task sees an already-cleared tag or a task that never held one.
            Kernel& k = kernel();
            KICKOS_DEBUG_ASSERT(k.task_holds > 0);
            k.task_holds--;
            if (t->refcount == 0)
            {
                free_task(t, held);
                return false;
            }
            // The domain reference this drops is the CREATOR's; the members' own, taken by the
            // first task_ref, keeps the domain alive.
            domain_release(t->domain);
            return true;
        }
    }

    bool task_sweep_done(Task* t)
    {
        if (t == nullptr or t->sweeping == 0)
        {
            return false;
        }
        // The count is this member's even where the slot was freed during the sweep: free_slot
        // does not seat a slot again until it is back to zero.
        t->sweeping--;
        free_put(t);
        return t->sweeping == 0 and t->refcount == 0;
    }

    uint16_t task_gen(Task const* t)
    {
        if (t == nullptr)
        {
            return 0;
        }
        return t->gen;
    }

    void task_report_death(Task* t, uint16_t gen, Held held)
    {
        if (t != nullptr and t->refcount == 0)
        {
            // Unequal only for a slot freed during the teardown, which free_task left its watch.
            bool const freed = t->gen != gen;
#if KICKOS_HAVE_ASPACE
            if (not freed)
            {
                if ((t->marks & TASK_MARK_ENDED) == 0)
                {
                    domain_retire_handles(t->domain);
                }
                domain_retire_space(t->domain);
            }
#endif
            uint8_t const fresh =
                static_cast<uint8_t>(TASK_MARK_DEAD | (~t->marks & TASK_MARK_ENDED));
            t->marks = static_cast<uint8_t>((t->marks & ~TASK_MARK_READY) | TASK_MARK_DEAD
                                            | TASK_MARK_ENDED);
            watch_raise(t, fresh);
            if (freed)
            {
                watch_clear(t);
            }
            else if ((t->marks & TASK_MARK_SLAIN) != 0)
            {
                task_drop_hold(t, held);
            }
        }
        // Also for a slot freed during the teardown: its sweep was the last thing in the way.
        start_watch_poke();
    }

    size_t task_image(Task const* t)
    {
        if (t != nullptr)
        {
            return t->image;
        }
        return 0;
    }

    int task_decl_in_the_way(size_t decl, Thread const* asker)
    {
        Task const& t = decl_slot(decl);
        bool const occupied = t.refcount != 0 or t.sweeping != 0;
        if (occupied and task_ended(&t))
        {
            return -KOS_EAGAIN;
        }
        if (occupied)
        {
            return -KOS_EBUSY;
        }
        // An instance whose entry thread has not been seated yet holds the record at refcount 0.
        if (t.creator_tag != ThreadPool::KILL_TAG_NONE and (t.marks & TASK_MARK_DEAD) == 0)
        {
            return -KOS_EBUSY;
        }
        if (retiring(t))
        {
            return -KOS_EAGAIN;
        }
#if KICKOS_PRESYNC
        // Another create is copying the data of its next space outside the lock.
        if (presync_image_staged_elsewhere(&t, asker))
        {
            return -KOS_EBUSY;
        }
#else
        (void)asker;
#endif
        return 0;
    }

    bool task_decl_lines_retiring(size_t decl)
    {
#if KICKOS_KERNEL_CORES > 1
        kos_task_decl const* const d = image_decl(decl);
        if (d == nullptr)
        {
            return false;
        }
        kos_decl_grant const* const g = image_decl_grants(d);
        for (uint16_t k = 0; k < d->grant_count; k++)
        {
            if (g[k].kind == KOS_DECL_LINE and irq_line_retiring(g[k].line))
            {
                return true;
            }
        }
#else
        (void)decl;
#endif
        return false;
    }

    bool task_decl_clear(size_t decl, Thread const* asker)
    {
        return task_decl_in_the_way(decl, asker) == 0 and not task_decl_lines_retiring(decl);
    }

    bool task_decl_spent(size_t decl)
    {
        // Where .data loads where it runs, its image keeps no copy of it.
        kos_task_decl const* const own = image_decl(decl);
        return (decl_slot(decl).marks & TASK_MARK_STARTED) != 0 and own != nullptr
               and (own->flags & (KOS_DECL_RESTARTS | KOS_DECL_ON_REQUEST)) == 0;
    }

    int task_decl_state(size_t decl)
    {
#if KICKOS_KERNEL_CORES > 1
        if (retiring(decl_slot(decl)))
        {
            irq_retire_drain();
        }
#endif
        int const rc = task_decl_in_the_way(decl, sched::current());
        if (rc != 0)
        {
            return rc;
        }
        if (task_decl_spent(decl))
        {
            return -KOS_EPERM;
        }
        return 0;
    }

    Task* task_create_declared(size_t decl, size_t image, uint16_t creator_tag, void* mem_base,
                               size_t mem_size, uint32_t mem_attr, Domain* donor, int* err,
                               Held held)
    {
        *err = 0;
        if (creator_tag == ThreadPool::KILL_TAG_NONE)
        {
            *err = KOS_EPERM;
            return nullptr;
        }
        Task* const t = &decl_slot(decl);
        // The dead incarnation's creator hold ends here, and its handle with it, before its
        // successor's domain is claimed: a full domain pool holds the dead one's.
        task_drop_hold(t, held);
        Domain* const d = domain_for(DOM_CALLER_UNPRIVILEGED | dom_caller_image(image)
                                         | dom_caller_payer(slot_of(t)),
                                     mem_base, mem_size, mem_attr, donor, err);
        if (d == nullptr)
        {
            return nullptr;
        }
        uint8_t const started = static_cast<uint8_t>(t->marks & TASK_MARK_STARTED);
        task_fill(t, d, image);
        t->marks = started;
        task_hold(t, d, creator_tag);
        return t;
    }

    void task_mark_started(Task* t)
    {
        t->marks = static_cast<uint8_t>(t->marks | TASK_MARK_STARTED);
    }

    void task_ready_seat(Task* t, int ep_obj)
    {
        t->watch_ep = notify_bound_store(ep_obj);
    }

    Thread* task_note_receive(Task* t, int ep_obj)
    {
        if (t == nullptr or t->watch_ep == 0 or (t->marks & TASK_MARK_READY) != 0)
        {
            return nullptr;
        }
        if (t->watch_ep != notify_bound_store(ep_obj))
        {
            return nullptr;
        }
        t->marks = static_cast<uint8_t>(t->marks | TASK_MARK_READY);
        Notification* const n = watch_target(t);
        if (n == nullptr or not watch_asks(t, TASK_MARK_READY))
        {
            return nullptr;
        }
        return notify_raise_deferred(n, watch_mask(t));
    }

    void task_orphan_created_by(uint16_t tag, Held held)
    {
        Kernel& k = kernel();
        if (tag == ThreadPool::KILL_TAG_NONE or tag > k.threads.count())
        {
            return;
        }
        ThreadLinks& creator = k.threads.links()[tag - 1];
        while (creator.created_head != 0)
        {
            task_drop_hold(&k.tasks()[creator.created_head - 1], held);
        }
    }

    void task_drop_hold(Task* t, Held held)
    {
        // Last, for it can switch: the walker that marked these members may never resume to end
        // their parks, and no hold is left for a later slay to name the task by.
        if (release_hold(t, held) and task_ended(t))
        {
            task_cancel_group(t, held);
        }
    }

    void task_stop_for_slay(Task* t, Held held)
    {
        if ((t->marks & TASK_MARK_SLAIN) != 0)
        {
            return;
        }
        // Marked before the stop, which can switch: a death reported meanwhile drops the hold and
        // frees the slot, which may be seated again before this resumes.
        t->marks = static_cast<uint8_t>(t->marks | TASK_MARK_SLAIN);
        if (not stop_group(t, held))
        {
            return;
        }
        if (t->refcount == 0 and t->sweeping == 0)
        {
            task_drop_hold(t, held);
            return;
        }
        // No walk: the stop's own ended every park, and a marked member parks no more.
        start_unstarted_slain(t, held);
    }

    void task_ref(Task* t)
    {
        if (t == nullptr)
        {
            return;
        }
        if (t->refcount == 0)
        {
            // The task, not the thread, is the domain's holder: one reference for the whole
            // group, taken when the group becomes non-empty.
            domain_ref(t->domain);
        }
        KICKOS_DEBUG_ASSERT(t->refcount < KICKOS_THREAD_SLOT_CEILING);
        KICKOS_DEBUG_ASSERT(not task_ended(t));
        t->refcount++;
    }

    uint8_t task_prio_ceiling(Task const* t)
    {
        if (t == nullptr or t->prio_ceiling == 0)
        {
            return KICKOS_PRIO_MAX;
        }
        return t->prio_ceiling;
    }

#if KICKOS_KERNEL_CORES > 1
    uint32_t task_core_set(Task const* t)
    {
        if (t == nullptr or t->core_set == 0)
        {
            return KICKOS_CORE_SET_ALL;
        }
        return t->core_set;
    }

    uint32_t task_default_cores(Task const* t)
    {
        uint32_t const grant = task_core_set(t);
        uint32_t const open = grant & ~static_cast<uint32_t>(KICKOS_ISOLATED_CORES);
        if (open == 0)
        {
            return grant;
        }
        return open;
    }
#endif

    int task_sched_narrow(Task* t, uint8_t ceiling, uint32_t cores)
    {
        if (t == nullptr)
        {
            return -KOS_EPERM;
        }
        if (ceiling != 0)
        {
            if (ceiling > task_prio_ceiling(t))
            {
                return -KOS_EPERM;
            }
        }
#if KICKOS_KERNEL_CORES > 1
        uint32_t admitted = 0;
        if (cores != 0)
        {
            int const crc = sched_admit_mask(cores, task_core_set(t), MaskBound::SUBSET,
                                             &admitted);
            if (crc != 0)
            {
                return crc;
            }
        }
#else
        (void)cores;
#endif
        if (ceiling != 0)
        {
            t->prio_ceiling = ceiling;
        }
#if KICKOS_KERNEL_CORES > 1
        if (cores != 0)
        {
            t->core_set = admitted;
        }
#endif
        return 0;
    }

    bool task_same_group(Thread const* a, Thread const* b)
    {
        if (a == nullptr or b == nullptr or a->task == nullptr)
        {
            return false;
        }
        return a->task == b->task;
    }

    TaskThreads task_member_count(Task const* t)
    {
        if (t == nullptr)
        {
            return 0;
        }
        return t->refcount;
    }

    TaskThreads task_sweeping(Task const* t)
    {
        if (t == nullptr)
        {
            return 0;
        }
        return t->sweeping;
    }

    void task_member_join(Task* t, Thread* th, int slot)
    {
        th->task = t;
        if (t == nullptr or slot < 0)
        {
            return;
        }
        t->members.push(&th->member);
    }

    void task_member_leave(Thread* th, int slot)
    {
        Task* const t = th->task;
        th->task = nullptr;
        if (t == nullptr or slot < 0 or not t->members.holds(&th->member))
        {
            return;
        }
        t->members.unlink(&th->member);
    }

    void task_run_join(Task* t, int slot)
    {
        int const i = slot_of(t);
        if (i < 0 or slot < 0)
        {
            return;
        }
        task_links()[i].cap_runs.push(&kernel().threads.slots()[slot].reclaim);
    }

    void task_run_leave(Task* t, int slot)
    {
        int const i = slot_of(t);
        if (i < 0 or slot < 0)
        {
            return;
        }
        HeadList& runs = task_links()[i].cap_runs;
        ListNode* const n = &kernel().threads.slots()[slot].reclaim;
        if (runs.holds(n))
        {
            runs.unlink(n);
        }
    }

    ListNode* task_runs(Task const* t)
    {
        int const i = slot_of(t);
        if (i < 0)
        {
            return nullptr;
        }
        return task_links()[i].cap_runs.head;
    }

    ListNode const* task_run_first(Thread const* th)
    {
        ListNode const* first = &th->reclaim;
        while (first->prev != nullptr)
        {
            first = first->prev;
        }
        return first;
    }

    Thread* task_built(int handle)
    {
        Thread* const t = kernel().threads.resolve(handle);
        if (t == nullptr or t->state != ThreadState::INACTIVE)
        {
            return nullptr;
        }
        return t;
    }

#if KICKOS_PRESYNC
    void task_abandon_building(Held held)
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr or r->building == 0)
        {
            return;
        }
        Thread* const t = task_built(static_cast<int>(r->building - 1u));
        r->building = 0;
        if (t == nullptr)
        {
            return;
        }
        t->task_role = TASK_ROLE_MEMBER;
        // Added first: a thread not yet added takes no cancel.
        sched::add(t, held);
        thread_cancel_kind(t, CANCEL_SLAY, held);
    }
#endif

    bool task_sheds(Task const* t)
    {
        if (not task_ended(t))
        {
            return false;
        }
        for (ListNode* m = t->members.head; m != nullptr; m = m->next)
        {
            if (not KICKOS_CONTAINER_OF(m, Thread, member)->dying)
            {
                return false;
            }
        }
        return true;
    }

    ListNode* task_members(Task const* t)
    {
        if (t == nullptr)
        {
            return nullptr;
        }
        return t->members.head;
    }

    int task_decl_of(Task const* t)
    {
        int const i = slot_of(t);
        if (i < shared_slots())
        {
            return -1;
        }
        return kernel().tasks().size() - 1 - i;
    }

#if KICKOS_HAVE_ASPACE
    HeadList* task_runs_head(Task const* t)
    {
        int const i = slot_of(t);
        if (i < 0)
        {
            return nullptr;
        }
        return &task_links()[i].runs;
    }

    HeadList* task_domains_head(Task const* t)
    {
        int const i = slot_of(t);
        if (i < 0)
        {
            return nullptr;
        }
        return &task_links()[i].domains;
    }

    ThreadLink* task_image_stager(Task const* t)
    {
        int const i = slot_of(t);
        if (i < 0)
        {
            return nullptr;
        }
        return &task_links()[i].image_stager;
    }

#endif

    Task* task_decl_record(size_t decl)
    {
        if (decl >= image_decl_count())
        {
            return nullptr;
        }
        return &decl_slot(decl);
    }

    Task* task_decl_dying(size_t decl)
    {
        Task& t = decl_slot(decl);
        if ((t.refcount != 0 or t.sweeping != 0) and task_ended(&t))
        {
            return &t;
        }
        return nullptr;
    }

#if KICKOS_KERNEL_CORES > 1
    HeadList* task_irq_retiring(Task const* t)
    {
        int const i = slot_of(t);
        if (i < 0)
        {
            return nullptr;
        }
        return &task_links()[i].irq_retiring;
    }

    void task_irq_retired(Task* t)
    {
        free_put(t);
    }
#endif

#if not KICKOS_HAVE_ASPACE
    uint16_t* task_ram_head(Task const* t)
    {
        int const i = slot_of(t);
        if (i < 0)
        {
            return nullptr;
        }
        return &task_links()[i].ram;
    }
#endif

    uint32_t task_budget(Task const* t, BudgetKind kind)
    {
        if (t == nullptr or kind == NO_BUDGET)
        {
            return 0;
        }
        size_t const k = static_cast<size_t>(kind);
        int const decl = task_decl_of(t);
        if (decl >= 0)
        {
            kos_task_decl const* const d = image_decl(static_cast<size_t>(decl));
            if (d == nullptr)
            {
                return 0;
            }
            return d->budgets[k];
        }
        // A record the boot seats: root's, whose budgets are the init's, or an idle task's, which
        // allocates nothing.
        return kickos_kernel_pools.init_budgets[k];
    }

    uint32_t task_cap_width(Task const* t)
    {
        int const decl = task_decl_of(t);
        if (decl >= 0)
        {
            kos_task_decl const* const d = image_decl(static_cast<size_t>(decl));
            if (d != nullptr)
            {
                return d->cap_width;
            }
        }
        return cap_root_width();
    }

    uint32_t task_usage(Task const* t, BudgetKind kind)
    {
        if (t == nullptr)
        {
            return 0;
        }
        // No default: a kind added to <kickos/sys/budget.h> without its derivation here fails
        // -Wswitch.
        switch (kind)
        {
        case BudgetKind::THREADS:
        {
            // Every slot the task occupies from the claim until it is reclaimable: a spawn
            // claims and joins under one lock hold, and a released member counts in `sweeping`
            // until its capability sweep is done and its slot retired.
            return static_cast<uint32_t>(t->refcount) + t->sweeping;
        }
        case BudgetKind::RAM_BYTES:
        case BudgetKind::RAM_BLOCKS:
        {
#if KICKOS_HAVE_ASPACE
            uint32_t ram = aspace_task_usage(task_domain(t), kind);
#if KICKOS_PRESYNC
            ram = ram + presync_task_staged(t, kind);
#endif
            return ram;
#else
            return task_ram_usage(t, kind);
#endif
        }
        case BudgetKind::PAGES:
        {
#if KICKOS_HAVE_ASPACE
            uint32_t pages = task_object_usage(t, kind) + domain_pages_paid(t);
#if KICKOS_PRESYNC
            pages = pages + presync_task_staged(t, kind) + t->tables_reserved;
#endif
            return pages;
#else
            return 0;
#endif
        }
        case BudgetKind::ENDPOINTS:
        case BudgetKind::NOTIFICATIONS:
        case BudgetKind::IRQ_BINDINGS:
        case BudgetKind::FRAME_RUNS:
        {
            return task_object_usage(t, kind);
        }
        case BudgetKind::CAP_SLOTS:
        {
            // Every run on the record's list, from its attach to the end of its thread's sweep.
            int const i = slot_of(t);
            if (i < 0)
            {
                return 0;
            }
            uint32_t used = 0;
            for (ListNode* n = task_links()[i].cap_runs.head; n != nullptr; n = n->next)
            {
                used = used + kcap_width_for(KICKOS_CONTAINER_OF(n, Thread, reclaim)->cap_width);
            }
            return used;
        }
        case BudgetKind::COUNT:
        {
            return 0;
        }
        }
        return 0;
    }

    bool task_admit(Task const* t, BudgetKind kind, uint32_t n)
    {
        if (t == nullptr)
        {
            return true;
        }
        uint32_t const budget = task_budget(t, kind);
        uint32_t const used = task_usage(t, kind);
        return used <= budget and n <= budget - used;
    }

#if not KICKOS_HAVE_ASPACE
    uint32_t task_ram_usage(Task const* t, BudgetKind kind)
    {
        uint16_t const* const head = task_ram_head(t);
        if (head == nullptr)
        {
            return 0;
        }
        RamBlock const* const blocks = pool_local(kickos_kernel_pools.ram_owners);
        uint32_t used = 0;
        for (uint16_t at = *head; at != 0; at = blocks[at - 1].next)
        {
            if (kind == BudgetKind::RAM_BLOCKS)
            {
                used++;
            }
            else
            {
                used = used + blocks[at - 1].size;
            }
        }
        return used;
    }
#endif

    bool task_release(Task* t, Held held)
    {
        if (t == nullptr or t->refcount == 0)
        {
            return false;
        }
        t->refcount--;
        t->sweeping++;
        if (t->refcount != 0)
        {
            return false;
        }
        if (t->creator_tag != ThreadPool::KILL_TAG_NONE)
        {
            // Emptied, not dead: the creator can still spawn into it. Its domain reference
            // is the creator's now, so the members' is the one released here.
            domain_release(t->domain);
            return true;
        }
        free_task(t, held);
        return true;
    }
}
