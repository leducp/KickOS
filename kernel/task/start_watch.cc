// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/start_watch.h>

#include <kickos/cap.h>
#include <kickos/debug.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        // A link names an entry by its thread's pool slot plus one and its index, so neither 0
        // nor LINK_END ever names one. Kernel::start_watches starts at 0; an armed entry's
        // obj_gen holds LINK_END for the last, and 0 marks an entry no list holds.
        constexpr uint32_t LINK_END = 0xFFFFFFFFu;
        constexpr uint32_t LINK_INDEX_MASK = 0xFFFFu;
        static_assert(KICKOS_THREAD_SLOT_CEILING < 0xFFFF and KCAP_WIDTH_CEILING <= LINK_INDEX_MASK,
                      "a link packs a thread slot plus one and a capability index in 16 bits each");

        bool names(uint32_t link)
        {
            return link != 0 and link != LINK_END;
        }

        uint32_t link_of(int slot, uint32_t index)
        {
            return (static_cast<uint32_t>(slot + 1) << 16) | index;
        }

        // An entry's address holds from its install to its close: a run is detached only once
        // its thread's sweep has emptied it.
        CapEntry* entry_of(uint32_t link)
        {
            Thread const& th = kernel().threads.slots()[static_cast<int>(link >> 16) - 1];
            return cap_slot(th.caps, link & LINK_INDEX_MASK);
        }

        size_t decl_of(CapEntry const& e)
        {
            return cap_watch_decl(e);
        }

        // Never null while the entry stands: its reference pins the object.
        Notification* target_of(CapEntry const& e)
        {
            return kernel().notifies.resolve(e.obj);
        }

        uint32_t mask_of(CapEntry const& e)
        {
            return 1u << notify_cap_bit(e);
        }

        // Whether the incarnation `decl`'s record holds has ended with threads still to die.
        bool lends(size_t decl)
        {
            Task const* const r = task_decl_record(decl);
            return task_ended(r) and (task_member_count(r) != 0 or task_sweeping(r) != 0);
        }

        void unlink(CapEntry* e)
        {
            uint32_t* at = &kernel().start_watches;
            while (names(*at))
            {
                CapEntry* const x = entry_of(*at);
                if (x == e)
                {
                    *at = e->obj_gen;
                    e->obj_gen = 0;
                    return;
                }
                at = &x->obj_gen;
            }
        }
    }

    int start_watch_arm(Thread* c, size_t decl, int notify_obj, uint32_t bit, uint32_t* out_cap,
                        Held held)
    {
        Kernel& k = kernel();
        int const slot = k.threads.index_of(c);
        KICKOS_ASSERT(slot >= 0);
        if (not obj_ref_inc(CapType::CAP_WATCH, notify_obj, 0))
        {
            return -KOS_EOVERFLOW;
        }
        uint32_t const index = cap_run_peek_free(c->cap_free_head);
        if (index == KCAP_NO_SLOT)
        {
            obj_ref_undo(CapType::CAP_WATCH, notify_obj, 0);
            return -KOS_EMFILE;
        }
        CapEntry* const e = cap_install_at(c, static_cast<int>(index), notify_obj,
                                           CapType::CAP_WATCH, 0, bit + 1u);
        cap_watch_decl_seat(e, static_cast<uint8_t>(decl));
        e->obj_gen = LINK_END;
        if (names(k.start_watches))
        {
            e->obj_gen = k.start_watches;
        }
        k.start_watches = link_of(slot, index);
        if (lends(decl))
        {
            notify_lend_mark(target_of(*e));
            death_wait_lend(task_decl_record(decl), held);
        }
        *out_cap = (static_cast<uint32_t>(e->gen) << KCAP_INDEX_BITS) | index;
        return 0;
    }

    void start_watch_close(CapEntry& e, Held held)
    {
        if (e.obj_gen == 0)
        {
            return;
        }
        unlink(&e);
        if (lends(decl_of(e)))
        {
            death_wait_lend(task_decl_record(decl_of(e)), held);
        }
    }

    void start_watch_poke()
    {
        IrqLock lock;
        Thread* top = nullptr;
        uint32_t* at = &kernel().start_watches;
        while (names(*at))
        {
            CapEntry* const x = entry_of(*at);
            if (not task_decl_clear(decl_of(*x), nullptr))
            {
                at = &x->obj_gen;
                continue;
            }
            *at = x->obj_gen;
            x->obj_gen = 0;
            // No switch before the walk is done: the list it stands on is shared.
            Thread* const woken = notify_raise_deferred(target_of(*x), mask_of(*x));
            if (woken != nullptr)
            {
                sched::keep_top(woken, &top, lock);
            }
        }
        if (top != nullptr)
        {
            sched::resched_after_wake(top, lock);
        }
    }

    bool start_watch_mark_decl(size_t decl)
    {
        bool any = false;
        for (uint32_t at = kernel().start_watches; names(at); at = entry_of(at)->obj_gen)
        {
            CapEntry const* const x = entry_of(at);
            if (decl_of(*x) == decl)
            {
                notify_lend_mark(target_of(*x));
                any = true;
            }
        }
        return any;
    }

    bool start_watch_owe_into(int notify_obj)
    {
        bool any = false;
        for (uint32_t at = kernel().start_watches; names(at); at = entry_of(at)->obj_gen)
        {
            CapEntry const* const x = entry_of(at);
            if (x->obj == notify_obj and lends(decl_of(*x)))
            {
                task_watch_lend_owe(task_decl_record(decl_of(*x)));
                any = true;
            }
        }
        return any;
    }

    uint8_t start_watch_lend(Task const* t, uint8_t p)
    {
        if (not names(kernel().start_watches))
        {
            return p;
        }
        int const decl = task_decl_of(t);
        if (decl < 0)
        {
            return p;
        }
        for (uint32_t at = kernel().start_watches; names(at); at = entry_of(at)->obj_gen)
        {
            CapEntry const* const x = entry_of(at);
            if (decl_of(*x) != static_cast<size_t>(decl))
            {
                continue;
            }
            Notification const* const n = target_of(*x);
            if (n == nullptr)
            {
                continue;
            }
            uint8_t const w = notify_lender_prio(n, mask_of(*x));
            if (w > p)
            {
                p = w;
            }
        }
        return p;
    }
}
