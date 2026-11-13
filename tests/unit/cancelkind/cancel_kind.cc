// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test monotonic cancellation (NONE < KILL < SLAY) and rejection of re-blocking
// for every nonzero kind. KILL must not restore a cleanup window after SLAY.
// Exercise notify_wait with the real notification code. Syscall death-point behavior is
// covered by sim_driver_death and thread_slay_window.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/notify.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/watch.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            uint32_t watch_spec(uint32_t kind, uint32_t events)
            {
                return (kind << KOS_WATCH_KIND_SHIFT) | events;
            }

            class CancelWiring : public KSeam
            {
            };

            constexpr int SLOT_KILLER = 0;
            constexpr int SLOT_VICTIM = 1;
            constexpr int SLOT_PEER = 2;

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_HIGH = 6;

            // Any in-range line does: what a claim reads is the dispatch slot, and no arm
            // here ever arms it.
            constexpr int CONSOLE_LINE = 14;
            constexpr uint32_t CAP_WIDTH = KICKOS_CAP_FIRST_DYNAMIC + 2;

            static_assert(CANCEL_NONE == 0,
                          "the thread_create memset must leave a fresh TCB un-cancelled");

            constexpr uint32_t LINE_BIT = 1u << 0;

            // The notification the arms below wait on, with the line attached to it on bit 0
            // and `owner` bound.
            void claim_the_line(Thread* owner)
            {
                attach_caps(owner, CAP_WIDTH);
                uint32_t line = 0;
                EXPECT_EQ(irq_claim(owner, CONSOLE_LINE, 0, &line), 0)
                    << "fixture: the waiter owns the line";
                uint32_t note = 0;
                EXPECT_EQ(notify_create(owner, &note), 0);
                EXPECT_EQ(watch_call(owner, watch_spec(KOS_WATCH_IRQ, KOS_IRQ_FIRED), line, note, 0), 0)
                    << "fixture: the line signals the object the waiter waits on";
                EXPECT_EQ(notify_bind(owner, note), 0)
                    << "fixture: the waiter is that object's bound thread";
            }

            // The waiter must be CURRENT, since notify_wait parks whoever calls it, and the peer
            // must be the only other runnable thread, so the scheduler picking it is what
            // resolves the park (kfixture.h note 2).
            Thread* seat_waiter_over_a_peer()
            {
                Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
                Thread* const waiter = seat_pool(SLOT_VICTIM, PRIO_HIGH);
                {
                    IrqLock lock;
                    sched::reschedule(nullptr, lock);
                }
                EXPECT_EQ(kernel().current(kickos_kernel_core()), waiter) << "fixture: the waiter is current";
                EXPECT_NE(peer, nullptr);
                g_switches = 0;
                trace_reset();
                return waiter;
            }

            // The event side of the wait: ends the park with the "consumed" result, which is
            // what notify_wait reads as a raise rather than a cancel.
            void hand_the_waiter_its_event(Thread* parked)
            {
                // Notification waits have no queue. The object identifies the waiter; the
                // bit distinguishes an event from cancellation.
                Notification* const n =
                    kernel().notifies.resolve(notify_bound_handle(parked->notify_bound));
                ASSERT_NE(n, nullptr) << "fixture: the waiter is bound to a live object";
                n->pending = n->pending | LINE_BIT;
                parked->clear_wait_edge();
                parked->wait_result = 0;
                {
                    IrqLock lock;
                    sched::wake(parked, lock);
                }
            }

            Notification* bound_object(Thread const* t)
            {
                return kernel().notifies.resolve(notify_bound_handle(t->notify_bound));
            }

            class NotifyWaitContract : public KSeam
            {
            };

            // Every TCB the kernel can reach, so an assertion of ABSENCE is over the whole
            // population rather than over the slots one arm happened to name.
            int slots_holding(uint8_t kind)
            {
                Kernel& k = kernel();
                int n = 0;
                for (int s = 0; s < k.threads.next; s++)
                {
                    if (k.threads.slots()[s].cancel_kind == kind)
                    {
                        n++;
                    }
                }
                return n;
            }
        }

        // --- what a kill writes -------------------------------------------------------

        TEST_F(CancelWiring, a_kill_writes_kill_exactly)
        {
            Thread* const victim = seat_pool(SLOT_VICTIM, PRIO_LOW);
            EXPECT_EQ(victim->cancel_kind, CANCEL_NONE) << "a fresh TCB is un-cancelled";

            {
                IrqLock lock;
                thread_cancel(victim, lock);
            }

            EXPECT_EQ(victim->cancel_kind, CANCEL_KILL)
                << "a kill is a KILL, and asserting merely non-zero here would accept a slay";
        }

        // One-way and idempotent: a second request must not escalate the kind, because the
        // difference between the kinds is whether the target keeps its cleanup window.
        TEST_F(CancelWiring, a_second_kill_does_not_escalate_the_kind)
        {
            Thread* const victim = seat_pool(SLOT_VICTIM, PRIO_LOW);

            {
                IrqLock lock;
                thread_cancel(victim, lock);
                thread_cancel(victim, lock);
            }

            EXPECT_EQ(victim->cancel_kind, CANCEL_KILL) << "still the kind the first call wrote";
        }

        TEST_F(CancelWiring, a_group_cancel_slays_every_member)
        {
            Task* const group = task(0);
            Thread* const a = seat_pool(SLOT_VICTIM, PRIO_LOW);
            Thread* const b = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(a, group);
            join_task(b, group);

            {
                IrqLock lock;
                task_cancel_group(group, lock);
            }

            EXPECT_EQ(a->cancel_kind, CANCEL_SLAY) << "the group form slays";
            EXPECT_EQ(b->cancel_kind, CANCEL_SLAY) << "every member, not just the first";
        }

        // --- the escalation order ------------------------------------------------------

        TEST_F(CancelWiring, a_slay_escalates_a_kill)
        {
            Thread* const victim = seat_pool(SLOT_VICTIM, PRIO_LOW);

            {
                IrqLock lock;
                thread_cancel_kind(victim, CANCEL_KILL, lock);
                thread_cancel_kind(victim, CANCEL_SLAY, lock);
            }

            EXPECT_EQ(victim->cancel_kind, CANCEL_SLAY)
                << "a supervisor that escalates from kill to slay must be able to: the whole "
                   "content of slay is denying a window the kill left open";
        }

        // The direction that matters for safety. Reverting to KILL would hand back a window
        // the caller of slay deliberately took away, and nothing downstream could tell.
        TEST_F(CancelWiring, a_kill_does_not_demote_a_slay)
        {
            Thread* const victim = seat_pool(SLOT_VICTIM, PRIO_LOW);

            {
                IrqLock lock;
                thread_cancel_kind(victim, CANCEL_SLAY, lock);
                thread_cancel_kind(victim, CANCEL_KILL, lock);
            }

            EXPECT_EQ(victim->cancel_kind, CANCEL_SLAY) << "still the stronger kind";
        }

        // Driven through the paths that DO write the byte rather than asserted over a pool
        // nothing touched. The FAULT exit is what makes the group cancel run: an ordinary
        // return of a member that is not the entry is scoped to one thread
        // (kernel/sched/sched.cc). The group is slain; the stranger killed directly keeps the
        // kill it was given.
        TEST_F(CancelWiring, a_fault_slays_its_group_and_a_kill_stays_a_kill)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_KILLER, PRIO_HIGH);
            Thread* const victim = seat_pool(SLOT_VICTIM, PRIO_LOW);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(victim, group);
            kernel().current(kickos_kernel_core()) = c;

            {
                IrqLock lock;
                thread_cancel(peer, lock);
            }
            run_exit_faulted(0);

            EXPECT_EQ(victim->cancel_kind, CANCEL_SLAY) << "a fault slays its group";
            EXPECT_EQ(slots_holding(CANCEL_SLAY), 1) << "and nobody outside it";
            EXPECT_EQ(slots_holding(CANCEL_KILL), 1) << "the direct kill landed as a kill";
        }

        // A SLAY IS AIMED AT ONE THREAD, so the byte it writes must not spread from the
        // victim's own exit. kos_task_slay is the verb for the group and writes the byte over
        // every member itself; a member's exit doing it too would leave a parent unable to
        // stop one worker without dying with it.
        TEST_F(CancelWiring, a_slain_members_exit_spares_its_peers)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_KILLER, PRIO_HIGH);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            kernel().current(kickos_kernel_core()) = c;
            c->cancel_kind = CANCEL_SLAY;

            run_exit(0);

            EXPECT_EQ(peer->cancel_kind, CANCEL_NONE)
                << "the victim's kind reaches the victim and stops there";
        }

        // --- what a reader does with the kind ------------------------------------------

        TEST_F(CancelWiring, the_reblock_refusal_reads_kill)
        {
            Thread* const waiter = seat_waiter_over_a_peer();
            claim_the_line(waiter);
            {
                IrqLock lock;
                thread_cancel(waiter, lock);
            }
            // Armed even on an arm that must NOT park: an unarmed park is a fixture exit(1)
            // that takes the rest of the suite with it, and a waker nothing consumed is
            // dropped at reset().
            wake_next_park(hand_the_waiter_its_event);

            EXPECT_EQ(notify_wait(waiter, LINE_BIT, KOS_LIMIT_NONE), -KOS_ECANCELED)
                << "a killed thread is refused the re-block";
            EXPECT_EQ(g_switches, 0u) << "and refused BEFORE parking, not woken out of one";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "it never took a wait edge";
        }

        // Seated by hand rather than through thread_cancel_kind, so the refusal is tested in
        // isolation from the escalation the arms above drive: a reader spelled
        // `== CANCEL_KILL` passes every other arm here while parking a thread whose resume is
        // already claimed.
        TEST_F(CancelWiring, the_reblock_refusal_is_total_over_the_kinds)
        {
            Thread* const waiter = seat_waiter_over_a_peer();
            claim_the_line(waiter);
            waiter->cancel_kind = CANCEL_SLAY;
            wake_next_park(hand_the_waiter_its_event); // as above: keep the failure an assertion

            EXPECT_EQ(notify_wait(waiter, LINE_BIT, KOS_LIMIT_NONE), -KOS_ECANCELED)
                << "every non-zero kind is refused, not just the cooperative one";
            EXPECT_EQ(g_switches, 0u) << "and refused before parking";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "it never took a wait edge";
        }

        // The converse, and the reason the two arms above are not satisfied by a refusal that
        // refuses everybody: an un-cancelled waiter still parks and still takes its event.
        TEST_F(CancelWiring, an_uncancelled_waiter_still_parks)
        {
            Thread* const waiter = seat_waiter_over_a_peer();
            claim_the_line(waiter);
            wake_next_park(hand_the_waiter_its_event);

            EXPECT_EQ(notify_wait(waiter, LINE_BIT, KOS_LIMIT_NONE), notify_wait_bits(LINE_BIT))
                << "CANCEL_NONE is not a death sentence, and the raise it parked for is what it "
                   "consumed";
            EXPECT_EQ(g_switches, 2u) << "the park switched away and the event switched back";
            EXPECT_EQ(waiter->cancel_kind, CANCEL_NONE) << "a park writes no kind";
        }

        // --- notify_wait's contract: the binding is the authority, the bits come back in the
        // result's high half -----------------------------------------------------------------

        // No waker is armed in the arms that must not park: a park ends the arm with the
        // fixture's refusal.
        TEST_F(NotifyWaitContract, an_empty_mask_is_refused_before_anything)
        {
            Thread* const waiter = seat_waiter_over_a_peer();
            claim_the_line(waiter);
            EXPECT_EQ(notify_wait(waiter, 0u, KOS_LIMIT_NONE), -KOS_EINVAL);
            EXPECT_EQ(g_switches, 0u);
        }

        TEST_F(NotifyWaitContract, a_thread_bound_to_nothing_is_refused)
        {
            Thread* const waiter = seat_waiter_over_a_peer();
            attach_caps(waiter, CAP_WIDTH);
            EXPECT_EQ(notify_wait(waiter, LINE_BIT, KOS_LIMIT_NONE), -KOS_EPERM);
            EXPECT_EQ(g_switches, 0u) << "a refused wait parked";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE);
        }

        TEST_F(NotifyWaitContract, a_cancel_outranks_a_missing_binding)
        {
            Thread* const waiter = seat_waiter_over_a_peer();
            attach_caps(waiter, CAP_WIDTH);
            {
                IrqLock lock;
                thread_cancel(waiter, lock);
            }
            EXPECT_EQ(notify_wait(waiter, LINE_BIT, KOS_LIMIT_NONE), -KOS_ECANCELED);
            EXPECT_EQ(g_switches, 0u);
        }

        TEST_F(NotifyWaitContract, a_passed_deadline_with_nothing_pending_times_out_at_once)
        {
            Thread* const waiter = seat_waiter_over_a_peer();
            claim_the_line(waiter);
            EXPECT_EQ(notify_wait(waiter, LINE_BIT, KOS_POLL.ns), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_switches, 0u) << "a passed deadline parked";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE);
            EXPECT_FALSE(waiter->on_timer) << "a passed deadline armed a timer";
        }

        TEST_F(NotifyWaitContract, the_taken_bits_come_back_in_the_high_half_and_leave_pending)
        {
            constexpr uint32_t HIGH_BIT = 1u << 31;
            constexpr uint32_t UNACCEPTED = 1u << 3;
            Thread* const waiter = seat_waiter_over_a_peer();
            claim_the_line(waiter);
            Notification* const n = bound_object(waiter);
            ASSERT_NE(n, nullptr);
            n->pending = LINE_BIT | HIGH_BIT | UNACCEPTED;

            int64_t const rc = notify_wait(waiter, LINE_BIT | HIGH_BIT, KOS_POLL.ns);

            EXPECT_EQ(rc, notify_wait_bits(LINE_BIT | HIGH_BIT))
                << "a pending bit waits for no deadline";
            EXPECT_EQ(kos_u64_lo(static_cast<uint64_t>(rc)), 0u) << "the low half is not 0";
            EXPECT_EQ(kos_u64_hi(static_cast<uint64_t>(rc)), LINE_BIT | HIGH_BIT);
            EXPECT_EQ(n->pending, UNACCEPTED)
                << "the taken bits stayed pending, or an unaccepted one went";
            EXPECT_EQ(g_switches, 0u);
        }
    }
}
