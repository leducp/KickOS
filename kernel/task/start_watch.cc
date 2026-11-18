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
        // A link names an entry by its thread's pool slot plus one in the high byte and its index
        // in the low one. LINK_NONE ends a list and marks an empty head; a high byte of 0xFF is
        // never seated.
        using Link = uint16_t;
        constexpr Link LINK_NONE = 0;
        constexpr uint32_t LINK_INDEX_MASK = 0xFFu;
        static_assert(KICKOS_THREAD_SLOT_CEILING < 0xFF and KCAP_WIDTH_CEILING <= 0xFF,
                      "a link packs a thread slot plus one and a capability index in a byte each");
        static_assert(sizeof(CapEntry::obj_gen) == 2 * sizeof(Link),
                      "an entry's obj_gen holds its two links");

        Link link_of(int slot, uint32_t index)
        {
            return static_cast<Link>((static_cast<uint32_t>(slot + 1) << 8) | index);
        }

        int slot_of(Link link)
        {
            return static_cast<int>(link >> 8) - 1;
        }

        // An entry's address holds from its install to its close: a run is detached only once
        // its thread's sweep has emptied it. `link` names one.
        CapEntry* entry_of(Link link)
        {
            Thread const& th = kernel().threads.slots()[slot_of(link)];
            return cap_slot(th.caps, link & LINK_INDEX_MASK);
        }

        Link next_of(CapEntry const& e)
        {
            return static_cast<Link>(e.obj_gen & 0xFFFFu);
        }

        Link prev_of(CapEntry const& e)
        {
            return static_cast<Link>(e.obj_gen >> 16);
        }

        void seat(CapEntry* e, Link prev, Link next)
        {
            e->obj_gen = (static_cast<uint32_t>(prev) << 16) | next;
        }

        Link& head_of(size_t decl)
        {
            return kernel().start_watches()[static_cast<int>(decl)];
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

        // A raised entry keeps its place, its badge KCAP_BADGE_NONE. On every list but the one
        // raise_decl walks, each armed entry precedes every raised one, so a walk for the armed
        // ones stops at the first raised one.
        bool armed(CapEntry const& e)
        {
            return cap_badge(e) != KCAP_BADGE_NONE;
        }

        // The armed entry `x` names, or null where the armed ones end.
        CapEntry* armed_at(Link x)
        {
            if (x == LINK_NONE)
            {
                return nullptr;
            }
            CapEntry* const e = entry_of(x);
            if (not armed(*e))
            {
                return nullptr;
            }
            return e;
        }

        void push(size_t decl, CapEntry* e, Link self)
        {
            Link& head = head_of(decl);
            seat(e, LINK_NONE, head);
            if (head != LINK_NONE)
            {
                CapEntry* const first = entry_of(head);
                seat(first, self, next_of(*first));
            }
            head = self;
        }

        void unlink(size_t decl, CapEntry* e)
        {
            Link const prev = prev_of(*e);
            Link const next = next_of(*e);
            if (prev == LINK_NONE)
            {
                head_of(decl) = next;
            }
            else
            {
                CapEntry* const before = entry_of(prev);
                seat(before, prev_of(*before), next);
            }
            if (next != LINK_NONE)
            {
                CapEntry* const after = entry_of(next);
                seat(after, prev, next_of(*after));
            }
            seat(e, LINK_NONE, LINK_NONE);
        }

        // Thread slot `slot`'s watch on `decl`, armed or raised, or LINK_NONE.
        Link own_link(size_t decl, int slot)
        {
            for (Link x = head_of(decl); x != LINK_NONE; x = next_of(*entry_of(x)))
            {
                if (slot_of(x) == slot)
                {
                    return x;
                }
            }
            return LINK_NONE;
        }

        // `decl` is clear: raise its armed watches. No switch before the walk is done: the list
        // it stands on is shared.
        void raise_decl(size_t decl, Thread** top, Held held)
        {
            Link x = head_of(decl);
            for (CapEntry* e = armed_at(x); e != nullptr; e = armed_at(x))
            {
                uint32_t const mask = mask_of(*e);
                cap_badge_seat(e, KCAP_BADGE_NONE);
                x = next_of(*e);
                Thread* const woken = notify_raise_deferred(target_of(*e), mask);
                if (woken != nullptr)
                {
                    sched::keep_top(woken, top, held);
                }
            }
        }
    }

    int start_watch_arm(Thread* c, size_t decl, int notify_obj, uint32_t bit, uint32_t* out_cap,
                        Held held)
    {
        int const slot = kernel().threads.index_of(c);
        KICKOS_ASSERT(slot >= 0);
        Link self = own_link(decl, slot);
        bool same = false;
        if (self != LINK_NONE)
        {
            same = entry_of(self)->obj == notify_obj;
        }
        if (not same and not obj_ref_inc(CapType::CAP_WATCH, notify_obj, 0))
        {
            return -KOS_EOVERFLOW;
        }
        CapEntry* e = nullptr;
        if (self != LINK_NONE)
        {
            e = entry_of(self);
            int const prior = e->obj;
            unlink(decl, e);
            e->obj = notify_obj;
            cap_badge_seat(e, bit + 1u);
            // The handle the replaced watch was answered with stops naming the entry.
            e->gen++;
            if (not same)
            {
                notify_ref_drop(prior);
            }
        }
        else
        {
            uint32_t const index = cap_run_peek_free(c->cap_free_head);
            if (index == KCAP_NO_SLOT)
            {
                obj_ref_undo(CapType::CAP_WATCH, notify_obj, 0);
                return -KOS_EMFILE;
            }
            e = cap_install_at(c, static_cast<int>(index), notify_obj, CapType::CAP_WATCH, 0,
                               bit + 1u);
            cap_watch_decl_seat(e, static_cast<uint8_t>(decl));
            self = link_of(slot, index);
        }
        push(decl, e, self);
        Task* const dying = task_decl_dying(decl);
        if (dying != nullptr)
        {
            notify_lend_mark(target_of(*e));
            watch_lend_task(dying, held);
        }
        *out_cap = (static_cast<uint32_t>(e->gen) << KCAP_INDEX_BITS) | (self & LINK_INDEX_MASK);
        return 0;
    }

    void start_watch_close(CapEntry& e, Held held)
    {
        size_t const decl = cap_watch_decl(e);
        bool const lent = armed(e);
        unlink(decl, &e);
        if (not lent)
        {
            return;
        }
        Task* const dying = task_decl_dying(decl);
        if (dying != nullptr)
        {
            watch_lend_task(dying, held);
        }
    }

    void start_watch_poke()
    {
        IrqLock lock;
        Thread* top = nullptr;
        PoolSpan<Link> const heads = kernel().start_watches();
        for (int d = 0; d < heads.size(); d++)
        {
            if (armed_at(heads[d]) != nullptr and task_decl_clear(static_cast<size_t>(d), nullptr))
            {
                raise_decl(static_cast<size_t>(d), &top, lock);
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
        Link x = head_of(decl);
        for (CapEntry* e = armed_at(x); e != nullptr; e = armed_at(x))
        {
            notify_lend_mark(target_of(*e));
            any = true;
            x = next_of(*e);
        }
        return any;
    }

    bool start_watch_owe_into(int notify_obj)
    {
        bool any = false;
        PoolSpan<Link> const heads = kernel().start_watches();
        for (int d = 0; d < heads.size(); d++)
        {
            if (armed_at(heads[d]) == nullptr)
            {
                continue;
            }
            Task* const dying = task_decl_dying(static_cast<size_t>(d));
            Link x = heads[d];
            for (CapEntry* e = armed_at(x); dying != nullptr and e != nullptr; e = armed_at(x))
            {
                if (e->obj == notify_obj)
                {
                    task_watch_lend_owe(dying);
                    any = true;
                    break;
                }
                x = next_of(*e);
            }
        }
        return any;
    }

    uint8_t start_watch_lend(size_t decl)
    {
        uint8_t p = 0;
        Link x = head_of(decl);
        for (CapEntry* e = armed_at(x); e != nullptr; e = armed_at(x))
        {
            uint8_t const w = notify_lender_prio(target_of(*e), mask_of(*e));
            if (w > p)
            {
                p = w;
            }
            x = next_of(*e);
        }
        return p;
    }
}
