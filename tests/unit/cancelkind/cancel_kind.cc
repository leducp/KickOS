// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cancellation authority is ONE byte carrying a KIND (docs/design-kill-and-slay.md
// section 3.5). Two claims, and the second is the one no other gate makes:
//   * the authority is MONOTONIC over the enum's values (NONE < KILL < SLAY): a kill writes
//     CANCEL_KILL, a slay escalates it, and a kill arriving afterwards never hands back the
//     cleanup window the slay took away. A cooperative death still propagates KILL and never
//     SLAY, so the two verbs cannot be conflated by a group.
//   * every reader is TOTAL over the non-zero kinds: it asks "has this thread been asked to
//     die", never "was it asked to die THIS way". A reader written against CANCEL_KILL would
//     pass every arm in the tree today and silently stop honouring the kind added next.
//
// The reader reachable here is the re-block refusal in kernel/irq/irq.cc, which the K-seam
// compiles. The other one is the death point in kernel/syscall/syscall.cc: no syscall boundary
// exists on the host side of the seam, so the sim_driver_death gate and the selftest's
// task_group_kill arm are what witness it.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            // Not named for the enum: the fixture class name IS the ctest suite name, and a
            // class shadowing CancelKind would make every value below need qualifying.
            class CancelWiring : public KSeam
            {
            };

            constexpr int SLOT_KILLER = 0;
            constexpr int SLOT_VICTIM = 1;
            constexpr int SLOT_PEER = 2;

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_HIGH = 6;

            // Free on every board the fixture compiles for, and no arm here ever arms it:
            // what a claim reads is the dispatch slot, so any in-range number does.
            constexpr int CONSOLE_LINE = 14;
            constexpr uint32_t CAP_WIDTH = KICKOS_CAP_FIRST_DYNAMIC + 2;

            static_assert(CANCEL_NONE == 0,
                          "the thread_create memset must leave a fresh TCB un-cancelled");
            static_assert(CANCEL_KILL != CANCEL_NONE and CANCEL_SLAY != CANCEL_NONE,
                          "both kinds must read as a death sentence to a reader testing "
                          "against CANCEL_NONE");
            static_assert(CANCEL_KILL != CANCEL_SLAY, "the two kinds must be distinguishable");
            static_assert(sizeof(Thread::cancel_kind) == sizeof(uint8_t),
                          "one byte where one byte was: Thread has no tail padding, so a wider "
                          "authority would grow every TCB");

            // The line cap the arms below wait on, owned by `owner` and current.
            uint32_t claim_the_line(Thread* owner)
            {
                attach_caps(owner, CAP_WIDTH);
                uint32_t cap = 0;
                EXPECT_EQ(irq_claim(owner, CONSOLE_LINE, 0, &cap), 0)
                    << "fixture: the waiter owns the line";
                return cap;
            }

            // The waiter is CURRENT because irq_wait parks whoever calls it, and the second
            // thread is the only other runnable one so the scheduler must pick it, which is
            // what resolves the park (kfixture.h note 2).
            Thread* seat_waiter_over_a_peer()
            {
                Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
                Thread* const waiter = seat_pool(SLOT_VICTIM, PRIO_HIGH);
                sched::reschedule();
                EXPECT_EQ(kernel().current, waiter) << "fixture: the waiter is current";
                EXPECT_NE(peer, nullptr);
                g_switches = 0;
                trace_reset();
                return waiter;
            }

            // The event side of the wait: ends the park with the "consumed" result, which is
            // what irq_wait reads as a raise rather than a cancel.
            void hand_the_waiter_its_event(Thread* parked)
            {
                parked->wait_queue->unlink(&parked->link);
                parked->clear_wait_edge();
                parked->wait_result = 0;
                sched::wake(parked);
            }

            // Every TCB the kernel can reach, so an assertion of ABSENCE is over the whole
            // population rather than over the slots one arm happened to name.
            int slots_holding(uint8_t kind)
            {
                Kernel& k = kernel();
                int n = 0;
                for (int s = 0; s < k.threads.next; s++)
                {
                    if (k.threads.slots[s].cancel_kind == kind)
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
                thread_cancel(victim);
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
                thread_cancel(victim);
                thread_cancel(victim);
            }

            EXPECT_EQ(victim->cancel_kind, CANCEL_KILL) << "still the kind the first call wrote";
        }

        TEST_F(CancelWiring, a_group_cancel_writes_kill_on_every_member)
        {
            Task* const group = task(0);
            Thread* const a = seat_pool(SLOT_VICTIM, PRIO_LOW);
            Thread* const b = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(a, group);
            join_task(b, group);

            {
                IrqLock lock;
                task_cancel_group(group, CANCEL_KILL);
            }

            EXPECT_EQ(a->cancel_kind, CANCEL_KILL) << "the group form writes the same kind";
            EXPECT_EQ(b->cancel_kind, CANCEL_KILL) << "on every member, not just the first";
        }

        // --- the escalation order ------------------------------------------------------

        TEST_F(CancelWiring, a_slay_escalates_a_kill)
        {
            Thread* const victim = seat_pool(SLOT_VICTIM, PRIO_LOW);

            {
                IrqLock lock;
                thread_cancel_kind(victim, CANCEL_KILL);
                thread_cancel_kind(victim, CANCEL_SLAY);
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
                thread_cancel_kind(victim, CANCEL_SLAY);
                thread_cancel_kind(victim, CANCEL_KILL);
            }

            EXPECT_EQ(victim->cancel_kind, CANCEL_SLAY) << "still the stronger kind";
        }

        // Driven through the paths that DO write the byte rather than asserted over a pool
        // nothing touched: a sweep that finds no slay because no cancel ran at all would pass
        // on a broken kill.
        TEST_F(CancelWiring, a_cooperative_death_never_writes_slay)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_KILLER, PRIO_HIGH);
            Thread* const victim = seat_pool(SLOT_VICTIM, PRIO_LOW);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(victim, group);
            kernel().current = c;

            {
                IrqLock lock;
                thread_cancel(peer);
            }
            run_exit(0);

            EXPECT_EQ(slots_holding(CANCEL_SLAY), 0)
                << "an ordinary exit ends its group cooperatively; only a slain member "
                   "escalates its peers";
            EXPECT_EQ(slots_holding(CANCEL_KILL), 2)
                << "and the sweep is not vacuous; the direct kill and the group's both landed";
        }

        // The kind travels with the death, which is the whole of "the group dies by ONE rule".
        // A peer left at CANCEL_KILL here keeps a cleanup window its sibling was denied.
        TEST_F(CancelWiring, a_slain_members_exit_slays_its_peers)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_KILLER, PRIO_HIGH);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            kernel().current = c;
            c->cancel_kind = CANCEL_SLAY;

            run_exit(0);

            EXPECT_EQ(peer->cancel_kind, CANCEL_SLAY)
                << "the dying member's kind is what the group cancel carries";
        }

        // --- what a reader does with the kind ------------------------------------------

        TEST_F(CancelWiring, the_reblock_refusal_reads_kill)
        {
            Thread* const waiter = seat_waiter_over_a_peer();
            uint32_t const cap = claim_the_line(waiter);
            {
                IrqLock lock;
                thread_cancel(waiter);
            }
            // Armed on an arm that must NOT park: an unarmed park is a fixture exit(1), which
            // takes the rest of the suite with it. With a waker the failure is this arm's own
            // assertions, and reset() drops one that was never consumed.
            wake_next_park(hand_the_waiter_its_event);

            EXPECT_EQ(irq_wait(waiter, cap), -KOS_ECANCELED)
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
            uint32_t const cap = claim_the_line(waiter);
            waiter->cancel_kind = CANCEL_SLAY;
            wake_next_park(hand_the_waiter_its_event); // as above: keep the failure an assertion

            EXPECT_EQ(irq_wait(waiter, cap), -KOS_ECANCELED)
                << "every non-zero kind is refused, not just the cooperative one";
            EXPECT_EQ(g_switches, 0u) << "and refused before parking";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "it never took a wait edge";
        }

        // The converse, and the reason the two arms above are not satisfied by a refusal that
        // refuses everybody: an un-cancelled waiter still parks and still takes its event.
        TEST_F(CancelWiring, an_uncancelled_waiter_still_parks)
        {
            Thread* const waiter = seat_waiter_over_a_peer();
            uint32_t const cap = claim_the_line(waiter);
            wake_next_park(hand_the_waiter_its_event);

            EXPECT_EQ(irq_wait(waiter, cap), 0) << "CANCEL_NONE is not a death sentence";
            EXPECT_EQ(g_switches, 2u) << "the park switched away and the event switched back";
            EXPECT_EQ(waiter->cancel_kind, CANCEL_NONE) << "a park writes no kind";
        }
    }
}
