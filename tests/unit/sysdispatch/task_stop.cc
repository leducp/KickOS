// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A kill of the task serving the published console ends that task, and with it the console, at
// once: before its members have run to their deaths, which a slay may not outlast. A slay marks
// each member before it can first switch and ends every park before it returns; its hold goes
// with the task's death, at once where no member is left. A slay switched out inside its stop
// may resume on a slot freed and seated again, and touches nothing of the new task.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"
#include "syscall_internal.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    class TaskStop : public KSeam
    {
    };

    constexpr uint8_t PRIO_CREATOR = 8;
    constexpr uint8_t PRIO_MEMBER = 4;
    constexpr uint8_t PRIO_SENDER = 10;
    constexpr uint8_t PRIO_WAKE = 10;

    struct FirstSwitch
    {
        bool taken;
        Thread* incoming;
        uint8_t spinner_kind;
    };
    FirstSwitch g_first;
    Thread* g_spinner;

    void read_member_at_switch()
    {
        g_first.taken = true;
        g_first.incoming = sched::current();
        g_first.spinner_kind = g_spinner->cancel_kind;
    }
}

TEST_F(TaskStop, a_kill_ends_the_console_task_before_its_member_runs)
{
    Thread* const creator = seat_pool(0, PRIO_CREATOR);
    attach_caps(creator, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), creator);
    int err = 0;
    Task* served = nullptr;
    {
        IrqLock lock;
        served = kickos::testfix::task_held(kernel().threads.kill_tag_of(creator), &err, lock);
    }
    ASSERT_NE(served, nullptr) << "fixture: the create refused (" << err << ")";
    Thread* const member = seat_pool(1, PRIO_MEMBER);
    join_task(member, served);
    member->task_role = kickos::TASK_ROLE_ENTRY;
    Endpoint* const ep = endpoint();
    {
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
        ASSERT_TRUE(cap_console_publish(creator, handle, lock));
        cap_console_serve(served, lock);
    }
    ASSERT_EQ(ep->console, EP_CONSOLE_SERVED) << "fixture: the console is not served";

    ASSERT_EQ(task_slay(task_handle(served)), 0);
    EXPECT_NE(member->state, ThreadState::EXITED) << "fixture: the member already died";
    EXPECT_TRUE(task_ended(served)) << "the kill did not end the task";
    EXPECT_EQ(ep->console, EP_CONSOLE_ENDED) << "the console outlives the kill of its task";
}

// Nothing walks the group after the stop's own walk, and the hold outlives the slay until the
// last member's death, through which the slayer still reads the task's state.
TEST_F(TaskStop, a_slay_ends_every_park_and_keeps_the_hold_until_the_death)
{
    Thread* const creator = seat_pool(0, PRIO_CREATOR);
    attach_caps(creator, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), creator);
    int err = 0;
    Task* group = nullptr;
    {
        IrqLock lock;
        group = kickos::testfix::task_held(kernel().threads.kill_tag_of(creator), &err, lock);
    }
    ASSERT_NE(group, nullptr) << "fixture: the create refused (" << err << ")";
    Thread* const parked = seat_pool(1, PRIO_MEMBER);
    join_task(parked, group);
    park_sleeper(parked, UINT64_MAX / 2);

    kos_task_t const handle = task_handle(group);
    ASSERT_EQ(task_slay(handle), 0);

    EXPECT_EQ(parked->cancel_kind, CANCEL_SLAY);
    EXPECT_NE(parked->state, ThreadState::BLOCKED) << "a member stays parked behind the slay";
    int32_t status = 0;
    EXPECT_EQ(task_state_call(handle, &status), KOS_TASK_LIVE | KOS_TASK_ENDED)
        << "the hold went with the slay";
    EXPECT_EQ(task_slay(handle), 0) << "a second slay before the death";

    kernel().current(kickos_kernel_core()) = parked;
    run_exit(0);
    kernel().current(kickos_kernel_core()) = creator;

    EXPECT_EQ(task_state_call(handle, &status), -KOS_EBADF) << "the hold outlived the death";
    EXPECT_EQ(task_slay(handle), -KOS_EBADF);
    IrqLock lock;
    EXPECT_FALSE(task_created_by(group, kernel().threads.kill_tag_of(creator)));
}

// No member left: the death is already reported, so the hold goes with the slay.
TEST_F(TaskStop, a_slay_of_a_task_with_no_member_drops_the_hold_at_once)
{
    Thread* const creator = seat_pool(0, PRIO_CREATOR);
    attach_caps(creator, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), creator);
    int err = 0;
    Task* group = nullptr;
    {
        IrqLock lock;
        group = kickos::testfix::task_held(kernel().threads.kill_tag_of(creator), &err, lock);
    }
    ASSERT_NE(group, nullptr) << "fixture: the create refused (" << err << ")";
    kos_task_t const handle = task_handle(group);

    ASSERT_EQ(task_slay(handle), 0);

    int32_t status = 0;
    EXPECT_EQ(task_state_call(handle, &status), -KOS_EBADF) << "the hold outlived the slay";
    EXPECT_EQ(task_slay(handle), -KOS_EBADF);
}

// A slay runs on its caller: the console end's refusal of a sender above it is the first switch,
// and every member is marked ahead of it.
TEST_F(TaskStop, a_running_slayer_marks_every_member_before_the_end_s_first_wake)
{
    Thread* const creator = seat_pool(0, PRIO_CREATOR);
    attach_caps(creator, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), creator);
    int err = 0;
    Task* served = nullptr;
    {
        IrqLock lock;
        served = kickos::testfix::task_held(kernel().threads.kill_tag_of(creator), &err, lock);
    }
    ASSERT_NE(served, nullptr) << "fixture: the create refused (" << err << ")";
    Endpoint* const ep = endpoint();
    {
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
        ASSERT_TRUE(cap_console_publish(creator, handle, lock));
        cap_console_serve(served, lock);
    }
    g_spinner = seat_pool(1, PRIO_MEMBER);
    join_task(g_spinner, served);
    Thread* const sender = seat_pool(2, PRIO_SENDER);
    park_plain_sender(sender, ep);
    g_first = FirstSwitch{};
    run_at_next_switch(read_member_at_switch);

    ASSERT_EQ(task_slay(task_handle(served)), 0);

    ASSERT_TRUE(g_first.taken) << "fixture: the slayer never switched";
    ASSERT_EQ(g_first.incoming, sender) << "fixture: the first switch is not the refusal";
    EXPECT_EQ(g_first.spinner_kind, CANCEL_SLAY) << "a member is unmarked at the end's first wake";
}

namespace
{
    // A notification bound to `w` as notify_bind leaves it, its handle in *obj.
    Notification* note_bound_to(Thread* w, int* obj)
    {
        Kernel& k = kernel();
        int const i = k.notifies.alloc(w->task);
        Notification* const n = k.notifies.at(i);
        *n = Notification{};
        n->bound = w;
        k.notifies.refs()[i] = 1;
        *obj = k.notifies.handle_for(i);
        w->notify_bound = notify_bound_store(*obj);
        return n;
    }

    // `w` parked on bit 0 of its own notification, which `t`'s watch raises at its end.
    void park_on_end_watch(Thread* w, Task* t)
    {
        int obj = 0;
        Notification* const n = note_bound_to(w, &obj);
        IrqLock lock;
        task_watch_arm(t, obj, 0u, KOS_TASK_ENDED, lock);
        seat_blocked(w);
        detach_ready(w);
        n->accept = 1u;
        w->wait_queue = nullptr;
        w->wait_kind = WAIT_NOTIFY;
        w->wait_obj = n;
        w->wait_result = WAIT_RESULT_POISON;
    }

    struct Reuse
    {
        Thread* creator;
        Task* slain;
        Thread* victim;
        Task* seated;
        // The new task is seated with no member, which leaves only its hold to drop.
        bool bare;
        Thread* successor;
        Notification* seated_watch;
    };
    Reuse g_reuse;

    // As the thread the slay switched to: the slain task's last member dies, which drops the
    // hold and frees the slot, and the creator's next task is seated in it, watched for its end.
    void retire_and_reuse()
    {
        Thread* const incoming = sched::current();
        kernel().current(kickos_kernel_core()) = g_reuse.victim;
        run_exit(0);
        kernel().current(kickos_kernel_core()) = incoming;
        IrqLock lock;
        int err = 0;
        g_reuse.seated = task_held(kernel().threads.kill_tag_of(g_reuse.creator), &err, lock);
        if (g_reuse.seated != g_reuse.slain or g_reuse.bare)
        {
            return;
        }
        g_reuse.successor = seat_pool(4, PRIO_MEMBER);
        join_task(g_reuse.successor, g_reuse.seated);
        int obj = 0;
        g_reuse.seated_watch = note_bound_to(g_reuse.successor, &obj);
        task_watch_arm(g_reuse.seated, obj, 0u, KOS_TASK_ENDED, lock);
    }

    // The creator running, holding a task whose one member is the victim.
    void seat_slain(uint8_t victim_prio)
    {
        g_reuse = Reuse{};
        g_reuse.creator = seat_pool(0, PRIO_CREATOR);
        attach_caps(g_reuse.creator, KICKOS_SEAM_CAP_WIDTH);
        IrqLock lock;
        sched::reschedule(nullptr, lock);
        int err = 0;
        g_reuse.slain = task_held(kernel().threads.kill_tag_of(g_reuse.creator), &err, lock);
        g_reuse.victim = seat_pool(1, victim_prio);
        join_task(g_reuse.victim, g_reuse.slain);
    }

    void expect_seated_untouched()
    {
        ASSERT_EQ(g_reuse.seated, g_reuse.slain) << "fixture: the slot was not seated again";
        EXPECT_FALSE(task_ended(g_reuse.seated)) << "the slay ended the new task";
        if (not g_reuse.bare)
        {
            EXPECT_EQ(g_reuse.seated_watch->pending, 0u) << "the slay raised ENDED on the new task";
            EXPECT_EQ(g_reuse.successor->cancel_kind, CANCEL_NONE)
                << "the slay cancelled a member of the new task";
        }
        IrqLock lock;
        EXPECT_TRUE(task_created_by(g_reuse.seated, kernel().threads.kill_tag_of(g_reuse.creator)))
            << "the slay dropped the new task's hold";
    }
}

// The console end's refusal of a sender above the creator switches the slay out of its stop.
TEST_F(TaskStop, a_slay_switched_out_by_the_console_end_leaves_a_reused_slot_alone)
{
    seat_slain(PRIO_MEMBER);
    ASSERT_NE(g_reuse.slain, nullptr) << "fixture: the create refused";
    Endpoint* const ep = endpoint();
    {
        IrqLock lock;
        int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
        ASSERT_TRUE(cap_console_publish(g_reuse.creator, handle, lock));
        cap_console_serve(g_reuse.slain, lock);
    }
    Thread* const sender = seat_pool(2, PRIO_SENDER);
    park_plain_sender(sender, ep);
    run_at_next_switch(retire_and_reuse);

    ASSERT_EQ(task_slay(task_handle(g_reuse.slain)), 0);

    expect_seated_untouched();
}

// The raise of the slain task's own end watch, whose waiter outranks the creator, switches the
// slay out of its stop.
TEST_F(TaskStop, a_slay_switched_out_by_its_end_watch_leaves_a_reused_slot_alone)
{
    seat_slain(PRIO_MEMBER);
    ASSERT_NE(g_reuse.slain, nullptr) << "fixture: the create refused";
    Thread* const waiter = seat_pool(2, PRIO_WAKE);
    park_on_end_watch(waiter, g_reuse.slain);
    run_at_next_switch(retire_and_reuse);

    ASSERT_EQ(task_slay(task_handle(g_reuse.slain)), 0);

    EXPECT_NE(waiter->state, ThreadState::BLOCKED) << "fixture: the end watch was not raised";
    expect_seated_untouched();
}

// The abort of the victim's park, the victim outranking the creator, switches the slay out at the
// end of its stop.
TEST_F(TaskStop, a_slay_switched_out_by_a_member_s_abort_leaves_a_reused_slot_alone)
{
    seat_slain(PRIO_WAKE);
    ASSERT_NE(g_reuse.slain, nullptr) << "fixture: the create refused";
    g_reuse.bare = true;
    park_sleeper(g_reuse.victim, UINT64_MAX / 2);
    run_at_next_switch(retire_and_reuse);

    ASSERT_EQ(task_slay(task_handle(g_reuse.slain)), 0);

    expect_seated_untouched();
}

// A member reads its own task through KOS_TASK_NONE, ORPHANED once the thread that created it is
// gone.
TEST_F(TaskStop, a_member_reads_its_own_task_orphaned_once_its_creator_dies)
{
    Thread* const creator = seat_pool(0, PRIO_CREATOR);
    attach_caps(creator, KICKOS_SEAM_CAP_WIDTH);
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    ASSERT_EQ(sched::current(), creator);
    int err = 0;
    Task* group = nullptr;
    {
        IrqLock lock;
        group = kickos::testfix::task_held(kernel().threads.kill_tag_of(creator), &err, lock);
    }
    ASSERT_NE(group, nullptr) << "fixture: the create refused (" << err << ")";
    Thread* const member = seat_pool(1, PRIO_MEMBER);
    join_task(member, group);

    int32_t status = 0;
    kernel().current(kickos_kernel_core()) = member;
    EXPECT_EQ(task_state_call(KOS_TASK_NONE, &status), KOS_TASK_LIVE);
    kernel().current(kickos_kernel_core()) = creator;
    run_exit(0);
    kernel().current(kickos_kernel_core()) = member;
    EXPECT_EQ(task_state_call(KOS_TASK_NONE, &status), KOS_TASK_LIVE | KOS_TASK_ORPHANED)
        << "the member cannot tell its creator is gone";
}
