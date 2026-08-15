// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/task.h>

#include <kickos/debug.h> // KICKOS_DEBUG_ASSERT
#include <kickos/domain.h>
#include <kickos/instance.h>

#include <kickos/sys/errno.h>

#include <stdint.h> // UINT8_MAX

namespace kickos
{
    namespace
    {
        // A slot is free iff it holds no live thread AND nobody reserved it. There are no
        // immortal tasks: even idle and root each get one, so nothing here needs domain.cc's
        // immortal arm.
        Task* free_slot()
        {
            Kernel& k = kernel();
            for (int i = 0; i < KICKOS_MAX_TASKS; i++)
            {
                Task& t = k.tasks[i];
                if (t.refcount == 0 and t.creator_tag == ThreadPool::KILL_TAG_NONE)
                {
                    return &t;
                }
            }
            return nullptr;
        }

        // The slot and its domain go back, and the generation bump is what stops a handle
        // still naming this task from resolving onto its successor.
        void free_task(Task* t)
        {
            domain_release(t->domain);
            // Nulling makes the debris FAIL-CLOSED: task_domain answers null, which every
            // reader already handles, rather than a Domain* the pool may have re-handed.
            t->domain = nullptr;
            t->creator_tag = ThreadPool::KILL_TAG_NONE;
            t->gen++;
        }

        int index_of(Task const* t)
        {
            Kernel& k = kernel();
            for (int i = 0; i < KICKOS_MAX_TASKS; i++)
            {
                if (&k.tasks[i] == t)
                {
                    return i;
                }
            }
            return -1;
        }
    }

    // task_for must never be the refusal a caller sees first: thread_spawn resolves the
    // task BEFORE it claims a thread slot, so a pool one slot short would turn a
    // thread-pool exhaustion into a task-pool exhaustion one spawn earlier. Both answer
    // -KOS_ENOMEM, but only this bound keeps the two coincident. The +1 is idle, which
    // holds a TCB outside the pool (thread.h) and so a task outside KICKOS_THREAD_SLOTS.
    //
    // An EXPLICIT task holding no thread is a slot this floor does not budget, so an app that
    // creates tasks and never populates them can make a spawn refuse one earlier. That is the
    // app's own doing and it is a clean -KOS_ENOMEM either way.
    static_assert(KICKOS_MAX_TASKS >= KICKOS_THREAD_SLOTS + 1,
                  "the task pool must cover every TCB that can be live at once, or "
                  "task_for refuses a spawn the thread pool would still have seated");

    static_assert(KICKOS_THREAD_SLOTS + 1 <= UINT8_MAX,
                  "Task::refcount is uint8_t and counts live threads: every TCB that can be "
                  "live at once must fit it");

    // creator_tag is a kill tag TRUNCATED to a byte. KILL_TAG_NONE is 0 and survives that;
    // every other tag must stay distinct from every other AND from idle's KILL_TAG_BOOT,
    // whose low byte is 0xFF.
    static_assert(KICKOS_THREAD_SLOTS < UINT8_MAX,
                  "a pool slot's kill tag would alias another, or idle's boot tag, once "
                  "truncated into Task::creator_tag");

    static_assert(KICKOS_MAX_TASKS < (1 << TASK_INDEX_BITS),
                  "task handle index field too small for KICKOS_MAX_TASKS");

    static_assert(KICKOS_MAX_TASKS <= UINT16_MAX,
                  "Kernel::task_holds is uint16_t and counts the slots carrying a creator "
                  "tag: every task that can hold one at once must fit it");

    void task_init(void)
    {
        Kernel& k = kernel();
        for (int i = 0; i < KICKOS_MAX_TASKS; i++)
        {
            k.tasks[i] = Task{};
        }
        k.task_holds = 0;
    }

    // Every reader OUTSIDE this file must come through here, so the representation stays
    // changeable. Inside it, task_for seats the field and task_ref/task_release read it.
    Domain* task_domain(Task const* t)
    {
        if (t == nullptr)
        {
            return nullptr;
        }
        return t->domain;
    }

    Task* task_for(bool privileged, void* mem_base, size_t mem_size,
                   bool caller_authorized, int* err)
    {
        // FIRST, so every domain refusal keeps its own code and its own position: the dedup
        // scan must run before a task slot is spent, or a refused grant leaves debris the
        // next scan has to reason about.
        Domain* const d = domain_for(privileged, mem_base, mem_size, caller_authorized, err);
        if (d == nullptr)
        {
            return nullptr;
        }
        Task* const t = free_slot();
        if (t == nullptr)
        {
            *err = KOS_ENOMEM; // pool exhausted: retry later
            return nullptr;
        }
        uint16_t const gen = t->gen;
        *t = Task{};
        t->gen = gen;
        t->domain = d;
        return t;
    }

    Task* task_create(uint16_t creator_tag, void* mem_base, size_t mem_size,
                      bool caller_authorized, int* err)
    {
        *err = 0;
        if (creator_tag == ThreadPool::KILL_TAG_NONE)
        {
            *err = KOS_EPERM; // a creator that matches nobody could never name the result
            return nullptr;
        }
        Domain* const d =
            domain_for(/*privileged=*/false, mem_base, mem_size, caller_authorized, err);
        if (d == nullptr)
        {
            return nullptr;
        }
        Task* const t = free_slot();
        if (t == nullptr)
        {
            *err = KOS_ENOMEM;
            return nullptr;
        }
        uint16_t const gen = t->gen;
        *t = Task{};
        t->gen = gen;
        t->domain = d;
        // The hold, taken before the reservation is visible: an explicit task sits at
        // refcount 0 between create and its first spawn, and only this reference stops the
        // domain pool re-handing the slot underneath it.
        domain_ref(d);
        t->creator_tag = static_cast<uint8_t>(creator_tag);
        kernel().task_holds++;
        return t;
    }

    Task* task_resolve(kos_task_t handle)
    {
        unsigned const biased =
            static_cast<unsigned>(handle & ((1u << TASK_INDEX_BITS) - 1u));
        if (biased == 0u)
        {
            return nullptr; // KOS_TASK_NONE, or a word carrying no index at all
        }
        int const index = static_cast<int>(biased - 1u);
        if (index >= KICKOS_MAX_TASKS)
        {
            return nullptr;
        }
        Task* const t = &kernel().tasks[index];
        if (t->gen != static_cast<uint16_t>(handle >> TASK_INDEX_BITS))
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
        int const index = index_of(t);
        if (index < 0)
        {
            return KOS_TASK_NONE;
        }
        return (static_cast<kos_task_t>(t->gen) << TASK_INDEX_BITS)
               | static_cast<kos_task_t>(index + 1);
    }

    bool task_created_by(Task const* t, uint16_t tag)
    {
        if (t == nullptr or tag == ThreadPool::KILL_TAG_NONE)
        {
            return false;
        }
        return t->creator_tag == static_cast<uint8_t>(tag);
    }

    void task_orphan_created_by(uint16_t tag)
    {
        Kernel& k = kernel();
        // At zero no slot carries a creator tag, so the scan below could only find nothing.
        // It runs interrupt-masked at EVERY thread exit, and on rx72m under an IRQ-driven
        // UART that was enough to move an RR slice boundary, so the early return is
        // load-bearing and not a micro-optimisation.
        if (k.task_holds == 0 or tag == ThreadPool::KILL_TAG_NONE)
        {
            return;
        }
        for (int i = 0; i < KICKOS_MAX_TASKS; i++)
        {
            Task* const t = &k.tasks[i];
            if (t->creator_tag == static_cast<uint8_t>(tag))
            {
                task_drop_hold(t);
            }
        }
    }

    void task_drop_hold(Task* t)
    {
        if (t == nullptr or t->creator_tag == ThreadPool::KILL_TAG_NONE)
        {
            return;
        }
        t->creator_tag = ThreadPool::KILL_TAG_NONE;
        // The ONLY site that clears a LIVE creator tag, which is what keeps the count in step
        // with the pool: free_task's clear is reached from here, where the tag is already
        // NONE, and from task_release, which takes that arm only when there was no hold.
        Kernel& k = kernel();
        KICKOS_DEBUG_ASSERT(k.task_holds > 0);
        k.task_holds--;
        if (t->refcount == 0)
        {
            free_task(t);
            return;
        }
        // A member still holds the slot. The domain reference this drops is the CREATOR's;
        // the members' own, taken by the first task_ref, keeps the domain alive.
        domain_release(t->domain);
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
        KICKOS_DEBUG_ASSERT(t->refcount < UINT8_MAX);
        t->refcount++;
    }

    uint8_t task_member_count(Task const* t)
    {
        if (t == nullptr)
        {
            return 0;
        }
        return t->refcount;
    }

    bool task_release(Task* t)
    {
        if (t == nullptr or t->refcount == 0)
        {
            return false;
        }
        t->refcount--;
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
        free_task(t);
        return true;
    }
}
