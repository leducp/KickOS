// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Death scoped by kind, and the group kill.
//
// Three claims, and only one of them is a counter:
//   * the SCOPE boundary. A FAULT ends the whole group, because siblings share the address
//     space the faulting thread was writing and no caller is watching a fault. Every death
//     a caller asked for ends one thread; the caller that wants the group has kos_task_kill
//     and kos_task_slay, which cancel every member themselves. Both directions are
//     asserted: a boundary tested one way passes with the group cancel deleted, and tested
//     the other way passes with it unconditional.
//   * the REACH of a cancel. Every park has to end, whatever the primitive under it, or a
//     group kill is only a kill of the threads that happened to park somewhere cancellable.
//   * the ORDER of a member's death against its peers'. Asserted through the fixture's
//     ordered trace, because a counter oracle cannot fail on a reordering, and the thing
//     that would go wrong here is a wake landing before or after the wrong step.
//
// What this gate CANNOT witness: the death POINT. A cancelled thread ends at its next syscall
// entry (kernel/syscall/syscall.cc), and no syscall boundary exists on the host side of the
// K-seam. The arms below assert that a peer is marked and made RUNNABLE; that it then dies is
// the sim's sim_driver_death and the selftest's task arms.

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class TaskDeath : public KSeam
            {
            };

            // Pool slots, not fixture storage: the group scan and the exit sweep both walk
            // kernel().threads, so a member outside the pool is a member nothing can find.
            constexpr int SLOT_DYING = 0;
            constexpr int SLOT_PEER = 1;
            constexpr int SLOT_OTHER = 2;

            // Below the dying thread's, so a wake inside exit_current does NOT switch and the
            // trace stays about the cancel rather than about scheduling.
            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;
            constexpr uint8_t PRIO_HIGH = 6;
        }

        // The SCOPE claim: even a death that ends its whole group reaches that group only.
        TEST_F(TaskDeath, a_lone_member_takes_nobody)
        {
            Task* const alone = task(0);
            Task* const other = task(1);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const stranger = seat_pool(SLOT_OTHER, PRIO_LOW);
            join_task(c, alone);
            join_task(stranger, other);
            kernel().current[arch_cpu_id()] = c;

            run_exit_faulted(0);

            EXPECT_EQ(stranger->cancel_kind, CANCEL_NONE)
                << "a thread in a DIFFERENT task must not be cancelled by this exit";
            EXPECT_NE(stranger->state, ThreadState::EXITED) << "the stranger keeps running";
        }

        // --- the SCOPE boundary --------------------------------------------------------
        // An ORDINARY RETURN ends one thread. A sibling is a thread of the same process and
        // its own return is what ends it; the task ends when the last member leaves. This is
        // the arm that fails if the group cancel is unconditional, which is what it was while
        // every spawn was alone in its task and the two readings could not be told apart.
        TEST_F(TaskDeath, an_ordinary_return_spares_its_peers)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            Semaphore* const s = semaphore(nullptr);
            park_sem_waiter(peer, s);
            kernel().current[arch_cpu_id()] = c;

            run_exit(0);

            EXPECT_EQ(peer->cancel_kind, CANCEL_NONE)
                << "a sibling's ordinary return is not a request for this thread to die";
            EXPECT_EQ(peer->state, ThreadState::BLOCKED) << "and it is left parked where it was";
            EXPECT_EQ(peer->wait_kind, WAIT_SEM) << "with its wait edge intact";
            EXPECT_EQ(task_member_count(group), 1u) << "the group outlives the member that left";
        }

        // A COOPERATIVE KILL is aimed at ONE thread and takes nobody else: kos_task_kill is
        // the verb for the group and marks every member itself, so propagating here would make
        // a single-thread kill unexpressible.
        TEST_F(TaskDeath, a_cooperative_kill_spares_its_peers)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            Semaphore* const s = semaphore(nullptr);
            park_sem_waiter(peer, s);
            c->cancel_kind = CANCEL_KILL;
            kernel().current[arch_cpu_id()] = c;

            run_exit(0);

            EXPECT_EQ(peer->cancel_kind, CANCEL_NONE)
                << "cancelling a thread does not cancel its peers";
            EXPECT_EQ(peer->state, ThreadState::BLOCKED) << "and leaves them parked";
        }

        // A SLAY is aimed at ONE named thread and takes nobody else. kos_thread_slay and
        // kos_task_slay are two syscalls, and the second cancels every member itself; making
        // a member's slay reach the group would collapse them into one and leave "stop this
        // worker" unexpressible by its own parent.
        TEST_F(TaskDeath, a_slain_member_spares_its_peers)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            Semaphore* const s = semaphore(nullptr);
            park_sem_waiter(peer, s);
            c->cancel_kind = CANCEL_SLAY;
            kernel().current[arch_cpu_id()] = c;

            run_exit(0);

            EXPECT_EQ(peer->cancel_kind, CANCEL_NONE)
                << "slaying one member is not slaying the group";
            EXPECT_EQ(peer->state, ThreadState::BLOCKED) << "and leaves it parked";
        }

        // A FAULT ends the group, and this is the fault-isolation property: the faulting
        // thread's siblings share the address space it was writing when it died. Cooperative,
        // so a peer keeps the window it holds long enough to quiet its device.
        TEST_F(TaskDeath, a_fault_ends_the_whole_task)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            Semaphore* const s = semaphore(nullptr);
            park_sem_waiter(peer, s);
            kernel().current[arch_cpu_id()] = c;

            run_exit_faulted(0);

            EXPECT_EQ(peer->cancel_kind, CANCEL_KILL)
                << "a contained fault ends the group it was contained to";
            EXPECT_NE(peer->state, ThreadState::BLOCKED) << "and the park ends";
        }

        // --- the REACH of the cancel ---------------------------------------------------
        // Two members, one exit. The peer is marked AND made runnable, which is what carries
        // it to its own death point.
        TEST_F(TaskDeath, a_member_exit_cancels_its_peer)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            Semaphore* const s = semaphore(nullptr);
            park_sem_waiter(peer, s);
            kernel().current[arch_cpu_id()] = c;

            run_exit_faulted(0);

            EXPECT_NE(peer->cancel_kind, CANCEL_NONE)
                << "a task-mate is cancelled by its peer's exit";
            EXPECT_NE(peer->state, ThreadState::BLOCKED)
                << "the peer is off its wait queue and runnable, not left parked";
            EXPECT_EQ(peer->wait_kind, WAIT_NONE) << "the wait edge is cleared by the waker";
        }

        // A SEMAPHORE park has no error channel at all: sem_wait returns void and reads no
        // wait_result. The cancel must still reach it, and must leave the count alone so a
        // later post still hands its token to a genuine waiter.
        TEST_F(TaskDeath, a_semaphore_park_is_reached_and_its_count_untouched)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            Semaphore* const s = semaphore(nullptr);
            park_sem_waiter(peer, s);
            kernel().current[arch_cpu_id()] = c;

            run_exit_faulted(0);

            EXPECT_EQ(s->count, 0) << "no token was minted to unpark the waiter";
            EXPECT_TRUE(s->waiters.head == nullptr) << "the waiter is off the semaphore queue";
            EXPECT_EQ(peer->wait_result, -KOS_ECANCELED)
                << "the waker writes the reason even where the primitive cannot report it";
        }

        // An ENDPOINT park unwinds through the endpoint layer, which is a different code path
        // from the plain-queue kinds above.
        TEST_F(TaskDeath, an_endpoint_park_is_reached)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            Endpoint* const ep = endpoint();
            park_plain_sender(peer, ep);
            kernel().current[arch_cpu_id()] = c;

            run_exit_faulted(0);

            EXPECT_TRUE(ep->send_waiters.head == nullptr)
                << "the sender is unlinked from send_waiters";
            EXPECT_EQ(peer->wait_result, -KOS_ECANCELED) << "the sender learns it was cancelled";
            EXPECT_NE(peer->state, ThreadState::BLOCKED) << "and it is no longer parked";
        }

        // A SLEEPER is on the timer delta list and on no wait queue, so the deadline is the
        // thing that has to be dropped. A cancelled sleeper left on the list would be woken a
        // second time by the timer, into a thread that is already dying.
        TEST_F(TaskDeath, a_sleeping_peer_leaves_the_timer_list)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            park_sleeper(peer, 1000000u);
            kernel().current[arch_cpu_id()] = c;

            run_exit_faulted(0);

            EXPECT_FALSE(peer->on_timer) << "the cancel drops the sleeper's deadline";
            EXPECT_NE(peer->state, ThreadState::BLOCKED) << "the sleeper is no longer parked";
            EXPECT_EQ(peer->wait_kind, WAIT_NONE) << "and its wait edge is cleared";
        }

        // A MUTEX waiter donated its priority to the owner. Cancelling it must revert that, or
        // an owner stays boosted by a waiter that is gone, a priority inversion with no
        // waiter to justify it, and the kind of leak nothing later corrects.
        TEST_F(TaskDeath, cancelling_a_mutex_waiter_reverts_the_owners_boost)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_HIGH);
            Thread* const owner = seat_pool(SLOT_OTHER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            int handle = 0;
            Mutex* const m = own_mutex(owner, &handle);
            park_mutex_waiter(peer, m);
            // The donation the real mutex_lock would have made.
            sched::set_prio(owner, PRIO_HIGH);
            kernel().current[arch_cpu_id()] = c;

            run_exit_faulted(0);

            EXPECT_EQ(owner->prio, PRIO_LOW)
                << "the owner falls back to its base priority once the waiter is gone";
            EXPECT_TRUE(m->owner == owner) << "ownership is NOT transferred to a cancelled waiter";
            EXPECT_TRUE(m->waiters.head == nullptr) << "the waiter is off the mutex queue";
        }

        // ORDER, and this is the arm a counter cannot replace. The peer OUTRANKS the dying
        // thread, so the dying guard admits its wake and the switch lands before the rest of
        // the exit, the console reclaim being the next step that leaves a mark. Moving the
        // group cancel after the capability sweep reorders this string, and so does a guard
        // that suppresses the wake.
        TEST_F(TaskDeath, a_higher_priority_peer_is_switched_to_before_the_exit_completes)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_HIGH);
            join_task(c, group);
            join_task(peer, group);
            Semaphore* const s = semaphore(nullptr);
            park_sem_waiter(peer, s);
            kernel().current[arch_cpu_id()] = c;
            trace_reset();

            run_exit_faulted(0);

            EXPECT_STREQ(trace(), "switch10>11 reclaim")
                << "the cancelled peer runs before the rest of the exit";
        }

        // The other side of the same guard, and the reason the cancel is safe where it sits: a
        // peer at or below the dying thread's priority is made ready and NOT switched to, so
        // the exit's own final reschedule stays the single switch away. An unguarded wake would
        // put a switch in front of the reclaim here too, and the dying thread would finish its
        // teardown only when something else happened to yield.
        TEST_F(TaskDeath, an_equal_priority_peer_does_not_preempt_the_exit)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_MID);
            join_task(c, group);
            join_task(peer, group);
            Semaphore* const s = semaphore(nullptr);
            park_sem_waiter(peer, s);
            kernel().current[arch_cpu_id()] = c;
            trace_reset();

            run_exit_faulted(0);

            EXPECT_STREQ(trace(), "reclaim switch10>11")
                << "the exit completes first, then hands over once";
        }

        // The dying test in thread_cancel is the ONLY thing sparing a thread from its own
        // group's cancel, and it has to cover two directions: the thread whose exit issued the
        // cancel, and a peer already tearing down when a second member goes. Without it the
        // two mark each other, and a second task_release on a slot already at zero is how that
        // becomes a live task decremented to nothing.
        TEST_F(TaskDeath, a_dying_member_is_not_cancelled_by_its_own_group)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            c->dying = true;
            kernel().current[arch_cpu_id()] = peer;

            IrqLock lock;
            task_cancel_group(group, CANCEL_KILL);

            EXPECT_EQ(c->cancel_kind, CANCEL_NONE)
                << "a thread already running its own exit is left alone";
            EXPECT_NE(peer->cancel_kind, CANCEL_NONE)
                << "and the live member IS cancelled by the same scan";
        }

        // --- the group-empty wake (WAIT_TASK_EMPTY) -----------------------------------
        // "The group is empty" is a different condition from any member's death, and it is
        // reported by the release that CAUSED it rather than re-derived afterwards: an
        // implicit task's slot is freed inside that call, and a refcount already at zero
        // cannot say whose departure took it there.
        TEST_F(TaskDeath, the_last_member_out_wakes_a_group_waiter)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const waiter = seat_pool(SLOT_OTHER, PRIO_LOW);
            join_task(c, group);
            kernel().policy->on_remove(waiter);
            waiter->state = ThreadState::BLOCKED;
            waiter->wait_kind = WAIT_TASK_EMPTY;
            waiter->wait_obj = group;
            waiter->wait_result = WAIT_RESULT_POISON;
            kernel().current[arch_cpu_id()] = c;

            run_exit(0);

            EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "the wait edge is the waker's to clear";
            EXPECT_EQ(waiter->wait_result, 0) << "0 is GONE: the group is empty";
            EXPECT_NE(waiter->state, ThreadState::BLOCKED) << "and it is runnable again";
        }

        // The other half: the wake is keyed on the EMPTYING, not on a death, so a member
        // leaving a group that still holds peers wakes nobody. Keyed on the death,
        // kos_task_slay would answer "empty" with members still alive.
        TEST_F(TaskDeath, a_death_that_leaves_peers_wakes_no_group_waiter)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            Thread* const waiter = seat_pool(SLOT_OTHER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            kernel().policy->on_remove(waiter);
            waiter->state = ThreadState::BLOCKED;
            waiter->wait_kind = WAIT_TASK_EMPTY;
            waiter->wait_obj = group;
            waiter->wait_result = WAIT_RESULT_POISON;
            kernel().current[arch_cpu_id()] = c;

            run_exit(0);

            EXPECT_EQ(waiter->wait_kind, WAIT_TASK_EMPTY) << "still parked on its group";
            EXPECT_EQ(waiter->wait_result, WAIT_RESULT_POISON) << "and nobody wrote it a result";
        }

        // The waiter names a DIFFERENT group, so an emptying it did not ask about must not
        // reach it. Without the pointer compare the sweep would wake every group waiter.
        TEST_F(TaskDeath, an_emptying_wakes_only_the_waiter_on_that_group)
        {
            Task* const group = task(0);
            Task* const other = task(1);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const waiter = seat_pool(SLOT_OTHER, PRIO_LOW);
            join_task(c, group);
            kernel().policy->on_remove(waiter);
            waiter->state = ThreadState::BLOCKED;
            waiter->wait_kind = WAIT_TASK_EMPTY;
            waiter->wait_obj = other;
            waiter->wait_result = WAIT_RESULT_POISON;
            kernel().current[arch_cpu_id()] = c;

            run_exit(0);

            EXPECT_EQ(waiter->wait_kind, WAIT_TASK_EMPTY) << "another group's emptying is not its";
            EXPECT_EQ(waiter->wait_result, WAIT_RESULT_POISON) << "and wrote it no result";
        }

        // task_cancel_group over a task nothing joined, and over a null task: both are the
        // shapes an EMPTY explicit task presents, which is what kos_task_kill sees when its
        // group never got a member.
        TEST_F(TaskDeath, an_empty_group_cancels_nobody)
        {
            Task* const empty = task(0);
            Thread* const stranger = seat_pool(SLOT_OTHER, PRIO_LOW);
            kernel().current[arch_cpu_id()] = stranger;

            IrqLock lock;
            task_cancel_group(empty, CANCEL_KILL);
            task_cancel_group(nullptr, CANCEL_KILL);

            EXPECT_EQ(stranger->cancel_kind, CANCEL_NONE)
                << "a thread in no task at all is not a member";
        }
    }
}
