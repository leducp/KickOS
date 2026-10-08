// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/task.h>

#include <kickos/debug.h> // KICKOS_DEBUG_ASSERT
#include <kickos/aspace.h>
#include <kickos/cap.h>
#include <kickos/domain.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/notify.h>
#include <kickos/ramown.h>
#include <kickos/sched.h>
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
        constexpr uint8_t TASK_MARK_NONBLOCK = 1u << 5; // O_NONBLOCK on the console's fds

        // A slot is free iff it holds no live thread, nobody reserved it, and no released
        // member still sweeps under its name: such a member's capabilities count in the budget
        // of whichever task holds the slot. There are no immortal tasks: even idle and root
        // each get one, so nothing here needs domain.cc's immortal arm.
        Task* free_slot()
        {
            Kernel& k = kernel();
            for (int i = 0; i < KICKOS_MAX_TASKS; i++)
            {
                Task& t = k.tasks[i];
                if (t.refcount == 0 and t.creator_tag == ThreadPool::KILL_TAG_NONE
                    and t.sweeping == 0)
                {
                    return &t;
                }
            }
            return nullptr;
        }

        // A slot for `d`, which domain_for built. On a full pool `d` is released: a domain built
        // by a handoff holds a reference on its DONOR, and an abandoned one pins that donor's
        // space and every frame in it until the slot happens to be reused.
        Task* task_seat(Domain* d, int* err)
        {
            Task* const t = free_slot();
            if (t == nullptr)
            {
                domain_release(d);
                *err = KOS_ENOMEM; // pool exhausted: retry later
                return nullptr;
            }
            uint16_t const gen = t->gen;
            *t = Task{};
            t->gen = gen;
            t->domain = d;
            // A task whose members are all cancelled ends with this status.
            t->exit_status = KOS_EXIT_CANCELLED;
            return t;
        }

        // Disarm the watch. Caller holds IrqLock.
        void watch_clear(Task* t)
        {
            t->watch_notify = 0;
            t->watch_ep = 0;
            t->watch_bit = 0;
            t->marks = static_cast<uint8_t>(t->marks & ~TASK_MARK_READY);
        }

        // The watch's notification, if the object still resolves: the handle is generational,
        // so a freed slot seated again is never raised into.
        Notification* watch_target(Task const* t)
        {
            if (t->watch_notify == 0)
            {
                return nullptr;
            }
            return kernel().notifies.resolve(notify_bound_handle(t->watch_notify));
        }

        void watch_raise(Task const* t)
        {
            Notification* const n = watch_target(t);
            if (n != nullptr)
            {
                (void)notify_raise(n, 1u << t->watch_bit);
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

        void mark_ended(Task* t, int code, bool latch, Held held)
        {
            if ((t->marks & TASK_MARK_ENDED) != 0)
            {
                return;
            }
            t->marks = static_cast<uint8_t>(t->marks | TASK_MARK_ENDED);
            if (latch)
            {
                t->exit_status = code;
            }
            console_leave(t, held);
            watch_raise(t);
        }

        // The slot and its domain go back, and the generation bump is what stops a handle
        // still naming this task from resolving onto its successor.
        void free_task(Task* t, Held held)
        {
            console_leave(t, held);
            watch_clear(t);
            domain_release(t->domain);
            // Nulling makes the debris FAIL-CLOSED: task_domain answers null, which every
            // reader already handles, rather than a Domain* the pool may have re-handed.
            t->domain = nullptr;
            t->creator_tag = ThreadPool::KILL_TAG_NONE;
            t->gen++;
        }
    }

    // idle and root each resolve a task at boot (thread.cc, kmain.cc) and neither call carries
    // a route out of a refusal.
    static_assert(KICKOS_MAX_TASKS >= 2,
                  "the task pool must seat idle's task and root's, which boot cannot refuse");

    static_assert(KICKOS_THREAD_SLOTS < UINT8_MAX,
                  "Task::refcount and Task::sweeping are uint8_t and must fit every TCB live at "
                  "once, idle's included, and a pool slot's kill tag would alias another, or "
                  "idle's boot tag, once truncated into Task::creator_tag");

    static_assert(KICKOS_MAX_TASKS <= UINT16_MAX,
                  "the task handle's biased index field, or Kernel::task_holds, which is "
                  "uint16_t and counts the slots carrying a creator tag, is too small for "
                  "KICKOS_MAX_TASKS");

    void task_init(void)
    {
        Kernel& k = kernel();
        for (int i = 0; i < KICKOS_MAX_TASKS; i++)
        {
            k.tasks[i] = Task{};
        }
        k.task_holds = 0;
    }

    Domain* task_domain(Task const* t)
    {
        if (t == nullptr)
        {
            return nullptr;
        }
        return t->domain;
    }

    Task* task_for(uint32_t caller, void* mem_base, size_t mem_size, Domain* donor,
                   int* err)
    {
        // Admission must run before a task slot is spent, or a refused grant leaves debris.
        // 0 memory type: the spawn ABI has no field to ask for another.
        Domain* const d = domain_for(caller, mem_base, mem_size, 0u, donor, err);
        if (d == nullptr)
        {
            return nullptr;
        }
        return task_seat(d, err);
    }

    Task* task_create(uint16_t creator_tag, void* mem_base, size_t mem_size,
                      uint32_t mem_attr, Domain* donor, int* err)
    {
        *err = 0;
        if (creator_tag == ThreadPool::KILL_TAG_NONE)
        {
            *err = KOS_EPERM; // a creator that matches nobody could never name the result
            return nullptr;
        }
        Domain* const d = domain_for(DOM_CALLER_TASK, mem_base, mem_size, mem_attr, donor, err);
        if (d == nullptr)
        {
            return nullptr;
        }
        Task* const t = task_seat(d, err);
        if (t == nullptr)
        {
            return nullptr;
        }
        // The hold, taken before the reservation is visible: an explicit task sits at
        // refcount 0 between create and its first spawn, and only this reference stops the
        // domain pool re-handing the slot underneath it.
        domain_ref(d);
        t->creator_tag = static_cast<uint8_t>(creator_tag);
        kernel().task_holds++;
#if KICKOS_ARCH_ARENA_DCACHE and not KICKOS_HAVE_ASPACE
        // Not in domain_for, whose spawn chain is the deeper one and whose grant is always Normal.
        if (domain_region_count(d) != 0)
        {
            arch_mpu_region const* const r = domain_region_at(d, 0);
            grant_sync(nullptr, r->base, r->size, r->attr);
        }
#endif
        return t;
    }

    Task* task_resolve(kos_task_t handle)
    {
        // Unbiased, so KOS_TASK_NONE and every word carrying no index at all wrap out of range.
        uint32_t const index = handle_index(handle) - 1u;
        if (index >= static_cast<uint32_t>(KICKOS_MAX_TASKS))
        {
            return nullptr;
        }
        Task* const t = &kernel().tasks[index];
        if (t->gen != handle_gen(handle))
        {
            return nullptr;
        }
        if (t->creator_tag == ThreadPool::KILL_TAG_NONE)
        {
            return nullptr; // an implicit task is unnameable, and a freed slot has no creator
        }
        return t;
    }

    kos_task_t task_handle(Task const* t)
    {
        int const index = slot_index_of(kernel().tasks, t);
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
        return t->creator_tag == static_cast<uint8_t>(tag);
    }

    int task_watch_call(kos_task_t task, uint32_t notify_cap, uint32_t ready_ep)
    {
        IrqLock lock;
        Thread* const c = sched::current();
        Task* const t = task_resolve(task);
        if (t == nullptr)
        {
            return -KOS_EBADF;
        }
        if (not task_created_by(t, kernel().threads.kill_tag_of(c)))
        {
            return -KOS_EPERM;
        }
        if (notify_cap == KOS_CAP_NONE)
        {
            watch_clear(t);
            return 0;
        }
        // Everything is resolved before the old watch goes, so a refused re-arm keeps it.
        int err = 0;
        if (cap_resolve_e(c, notify_cap, CapType::CAP_NOTIFY, CAP_SIGNAL, &err) == nullptr)
        {
            return -err;
        }
        CapEntry const* const ne = cap_lookup(c, notify_cap);
        int32_t ep_stored = 0;
        if (ready_ep != KOS_CAP_NONE)
        {
            if (cap_resolve_e(c, ready_ep, CapType::CAP_ENDPOINT, 0, &err) == nullptr)
            {
                return -err;
            }
            ep_stored = notify_bound_store(cap_lookup(c, ready_ep)->obj);
        }
        // No reference: whoever can receive the report holds a capability or a binding, and
        // either keeps the object alive; a report nobody can receive has nowhere to go.
        watch_clear(t);
        t->watch_notify = notify_bound_store(ne->obj);
        t->watch_bit = static_cast<uint8_t>(notify_cap_bit(*ne));
        t->watch_ep = ep_stored;
        return 0;
    }

    int task_state_call(kos_task_t task)
    {
        IrqLock lock;
        Task* const t = task_resolve(task);
        if (t == nullptr)
        {
            return -KOS_EBADF;
        }
        if (not task_created_by(t, kernel().threads.kill_tag_of(sched::current())))
        {
            return -KOS_EPERM;
        }
        int state = t->marks & (TASK_MARK_READY | TASK_MARK_DEAD | TASK_MARK_ENDED);
        if (t->refcount != 0)
        {
            state |= KOS_TASK_LIVE;
        }
        return state;
    }

    int task_exit_status_call(kos_task_t task, int32_t* status)
    {
        IrqLock lock;
        Task* const t = task_resolve(task);
        if (t == nullptr)
        {
            return -KOS_EBADF;
        }
        if (not task_created_by(t, kernel().threads.kill_tag_of(sched::current())))
        {
            return -KOS_EPERM;
        }
        if ((t->marks & TASK_MARK_ENDED) == 0)
        {
            return -KOS_EBUSY;
        }
        *status = t->exit_status;
        return 0;
    }

    void task_end(Task* t, int code, bool latch, Held held)
    {
        if (t == nullptr)
        {
            return;
        }
        mark_ended(t, code, latch, held);
        // The ending thread is still a member, so a count of one leaves nobody to cancel.
        if (t->refcount > 1)
        {
            task_cancel_group(t, held);
        }
    }

    void task_stop(Task* t, Held held)
    {
        if (t == nullptr)
        {
            return;
        }
        mark_ended(t, 0, /*latch=*/false, held);
        task_cancel_group(t, held);
    }

    void task_console_serve(Task* t)
    {
        Kernel& k = kernel();
        for (int i = 0; i < KICKOS_MAX_TASKS; i++)
        {
            k.tasks[i].marks = static_cast<uint8_t>(k.tasks[i].marks & ~TASK_MARK_CONSOLE);
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

    bool task_nonblocking(Task const* t)
    {
        return t != nullptr and (t->marks & TASK_MARK_NONBLOCK) != 0;
    }

    int task_nonblock_call(int op)
    {
        IrqLock lock;
        Task* const t = sched::current()->task;
        if (t == nullptr)
        {
            return -KOS_EINVAL;
        }
        if (op == KOS_NONBLOCK_SET)
        {
            t->marks = static_cast<uint8_t>(t->marks | TASK_MARK_NONBLOCK);
        }
        else if (op == KOS_NONBLOCK_CLEAR)
        {
            t->marks = static_cast<uint8_t>(t->marks & ~TASK_MARK_NONBLOCK);
        }
        else if (op != KOS_NONBLOCK_GET)
        {
            return -KOS_EINVAL;
        }
        return static_cast<int>(task_nonblocking(t));
    }

    bool task_ended(Task const* t)
    {
        return t != nullptr and (t->marks & TASK_MARK_ENDED) != 0;
    }

    bool task_sweep_done(Task* t, uint16_t gen)
    {
        if (t == nullptr or t->sweeping == 0)
        {
            return false;
        }
        // The count is this member's even where the slot was freed during the sweep: free_slot
        // does not seat a slot again until it is back to zero.
        t->sweeping--;
        return t->gen == gen and t->sweeping == 0 and t->refcount == 0;
    }

    uint16_t task_gen(Task const* t)
    {
        if (t == nullptr)
        {
            return 0;
        }
        return t->gen;
    }

    void task_report_death(Task* t, uint16_t gen)
    {
        // A slot freed during the teardown and seated again is another instance's.
        if (t == nullptr or t->gen != gen or t->refcount != 0)
        {
            return;
        }
        t->marks = static_cast<uint8_t>((t->marks & ~TASK_MARK_READY) | TASK_MARK_DEAD
                                        | TASK_MARK_ENDED);
        watch_raise(t);
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
        if (n == nullptr)
        {
            return nullptr;
        }
        return notify_raise_deferred(n, 1u << t->watch_bit);
    }

    void task_orphan_created_by(uint16_t tag, Held held)
    {
        Kernel& k = kernel();
        // At zero no slot carries a creator tag. The scan below runs interrupt-masked at EVERY
        // thread exit, so this early return is load-bearing.
        if (k.task_holds == 0 or tag == ThreadPool::KILL_TAG_NONE)
        {
            return;
        }
        for (int i = 0; i < KICKOS_MAX_TASKS; i++)
        {
            Task* const t = &k.tasks[i];
            if (t->creator_tag == static_cast<uint8_t>(tag))
            {
                task_drop_hold(t, held);
            }
        }
    }

    void task_drop_hold(Task* t, Held held)
    {
        if (t == nullptr or t->creator_tag == ThreadPool::KILL_TAG_NONE)
        {
            return;
        }
        t->creator_tag = ThreadPool::KILL_TAG_NONE;
        // The only place a live creator tag is cleared, which keeps task_holds exact: free_task
        // sees an already-cleared tag or a task that never held one.
        Kernel& k = kernel();
        KICKOS_DEBUG_ASSERT(k.task_holds > 0);
        k.task_holds--;
        if (t->refcount == 0)
        {
            free_task(t, held);
            return;
        }
        // A member still holds the slot. The domain reference this drops is the CREATOR's;
        // the members' own, taken by the first task_ref, keeps the domain alive.
        domain_release(t->domain);
    }

#if KICKOS_HAVE_ASPACE
    void task_discard(Task* t, Held held)
    {
        if (t == nullptr or t->refcount != 0
            or t->creator_tag != ThreadPool::KILL_TAG_NONE)
        {
            return;
        }
        free_task(t, held);
    }
#endif

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
        KICKOS_DEBUG_ASSERT(t->refcount < UINT8_MAX);
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

    uint8_t task_member_count(Task const* t)
    {
        if (t == nullptr)
        {
            return 0;
        }
        return t->refcount;
    }

    uint8_t task_sweeping(Task const* t)
    {
        if (t == nullptr)
        {
            return 0;
        }
        return t->sweeping;
    }

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
