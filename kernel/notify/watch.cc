// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/watch.h>

#include <kickos/cap.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/notify.h>
#include <kickos/task.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        // Indexed by enum kos_watch_kind: the event bits each kind defines, 0 for no kind.
        constexpr uint16_t WATCH_EVENTS[] = {
            0u,
            KOS_TASK_READY | KOS_TASK_DEAD | KOS_TASK_ENDED,
            KOS_IRQ_FIRED,
        };
        constexpr uint32_t WATCH_KIND_COUNT = sizeof(WATCH_EVENTS) / sizeof(WATCH_EVENTS[0]);
        static_assert(KOS_WATCH_TASK == 1 and KOS_WATCH_IRQ == 2 and WATCH_KIND_COUNT == 3,
                      "WATCH_EVENTS is indexed by enum kos_watch_kind");
        constexpr uint32_t WATCH_FLAGS_MASK = 0xFFu;

        struct Target
        {
            Notification* n;
            int obj;
            uint8_t bit;
        };

        // The notification every kind raises, with CAP_SIGNAL; the capability's badge is the bit.
        int target_of(Thread* c, uint32_t notify_cap, Target* out)
        {
            int err = 0;
            Notification* const n = static_cast<Notification*>(
                cap_resolve_e(c, notify_cap, CapType::CAP_NOTIFY, CAP_SIGNAL, &err));
            if (n == nullptr)
            {
                return -err;
            }
            CapEntry const* const e = cap_lookup(c, notify_cap);
            out->n = n;
            out->obj = static_cast<int>(e->obj);
            out->bit = static_cast<uint8_t>(notify_cap_bit(*e));
            return 0;
        }

        int watch_task(Thread* c, uint32_t events, uint32_t source, uint32_t notify_cap)
        {
            int err = 0;
            Task* const t = task_watch_source(c, static_cast<kos_task_t>(source), &err);
            if (t == nullptr)
            {
                return -err;
            }
            if (events == 0)
            {
                task_watch_disarm(t);
                return 0;
            }
            if ((events & KOS_TASK_READY) != 0 and not task_watch_ready_named(t))
            {
                return -KOS_EINVAL;
            }
            // Resolved before the old watch goes, so a refused re-arm keeps it.
            Target to{};
            int const rc = target_of(c, notify_cap, &to);
            if (rc != 0)
            {
                return rc;
            }
            task_watch_arm(t, to.obj, to.bit, events);
            return 0;
        }

        int watch_irq(Thread* c, uint32_t events, uint32_t source, uint32_t notify_cap)
        {
            int err = 0;
            IrqBinding* const b = irq_watch_source(c, source, &err);
            if (b == nullptr)
            {
                return -err;
            }
            if (events == 0)
            {
                return -KOS_ENOTSUP;
            }
            if (irq_watch_bound(b))
            {
                return -KOS_EBUSY;
            }
            Target to{};
            int const rc = target_of(c, notify_cap, &to);
            if (rc != 0)
            {
                return rc;
            }
            return irq_watch_arm(b, to.n, to.obj, to.bit);
        }
    }

    int watch_call(Thread* c, uint32_t spec, uintptr_t source, uint32_t notify_cap, uintptr_t arg)
    {
        uint32_t const kind = spec >> KOS_WATCH_KIND_SHIFT;
        uint32_t const flags = (spec >> KOS_WATCH_FLAGS_SHIFT) & WATCH_FLAGS_MASK;
        uint32_t const events = spec & KOS_WATCH_EVENTS_MASK;
        if (kind >= WATCH_KIND_COUNT)
        {
            return -KOS_EINVAL;
        }
        uint32_t const known = WATCH_EVENTS[kind];
        if (known == 0 or flags != 0 or (events & ~known) != 0)
        {
            return -KOS_EINVAL;
        }
        if (arg != 0)
        {
            return -KOS_EINVAL;
        }
        if (events == 0 and kind == KOS_WATCH_TASK and notify_cap != KOS_CAP_NONE)
        {
            return -KOS_EINVAL;
        }
        uint32_t const handle = static_cast<uint32_t>(source);
        if (static_cast<uintptr_t>(handle) != source)
        {
            return -KOS_EBADF;
        }
        IrqLock lock;
        switch (static_cast<kos_watch_kind>(kind))
        {
            case KOS_WATCH_TASK:
            {
                return watch_task(c, events, handle, notify_cap);
            }
            case KOS_WATCH_IRQ:
            {
                return watch_irq(c, events, handle, notify_cap);
            }
        }
        return -KOS_EINVAL;
    }
}
