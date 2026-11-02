// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A task watch names its notification by a handle carrying 16 bits of the slot's generation. A
// watch armed on a notification whose slot is freed and reused 65536 times raises nothing into
// the slot's new occupant when its task ends.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    class StaleWatch : public KSeam
    {
    };

    constexpr uint32_t WRAP = 1u << 16;

    Task* new_task(Thread* creator)
    {
        IrqLock lock;
        int err = 0;
        return task_held(kernel().threads.kill_tag_of(creator), &err, lock);
    }

    void stop(Task* t)
    {
        IrqLock lock;
        task_stop(t, lock);
    }
}

TEST_F(StaleWatch, a_watch_kept_across_a_generation_wrap_of_its_notification_raises_nothing)
{
    Thread* const creator = seat_pool(0, 10);
    attach_caps(creator, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), creator);
    Task* const watched = new_task(creator);
    ASSERT_NE(watched, nullptr);
    uint32_t note = KCAP_INVALID;
    ASSERT_EQ(notify_create(creator, &note), 0);
    ASSERT_EQ(task_watch_call(task_handle(watched), note, KOS_CAP_NONE), 0);

    Kernel& k = kernel();
    int obj = 0;
    {
        IrqLock lock;
        obj = cap_lookup(creator, note)->obj;
    }
    int const slot = k.notifies.live_index(obj);
    ASSERT_GE(slot, 0);
    // Every other slot taken; the notification dies and its slot is reused until the watch's
    // 16 bits of generation come round.
    while (k.notifies.alloc(nullptr) >= 0)
    {
    }
    {
        IrqLock lock;
        notify_ref_drop(obj);
    }
    for (uint32_t i = 1; i < WRAP; i++)
    {
        ASSERT_EQ(k.notifies.alloc(nullptr), slot);
        k.notifies.free(k.notifies.handle_for(slot));
    }
    ASSERT_EQ(k.notifies.alloc(nullptr), slot);
    Notification* const n = k.notifies.at(slot);
    n->pending = 0;
    n->bound = nullptr;
    n->accept = 0;
    n->signallers = NOTIFY_SIGNALLER_NONE;
    k.notifies.refs()[slot] = 1;
    ASSERT_EQ(k.notifies.live_index(obj), slot) << "fixture: the watch's handle names the new occupant";

    stop(watched);
    EXPECT_EQ(n->pending, 0u) << "the watch raised the notification slot's new occupant";

    // The control: a watch armed on the occupant now raises it at its task's end.
    Task* const second = new_task(creator);
    ASSERT_NE(second, nullptr);
    uint32_t fresh = KCAP_INVALID;
    {
        IrqLock lock;
        ASSERT_EQ(cap_install(creator, k.notifies.handle_for(slot), CapType::CAP_NOTIFY, CAP_SIGNAL,
                              &fresh),
                  0);
    }
    ASSERT_EQ(task_watch_call(task_handle(second), fresh, KOS_CAP_NONE), 0);
    stop(second);
    EXPECT_EQ(n->pending, 1u) << "the control watch raised nothing";
}
