// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// cap_teardown drops IrqLock between chunks, so a second dying thread scheduled into that gap
// starts a sweep of its own.
//
// The claim these arms make is an ORDER; a counter oracle cannot fail on a reordering.
//
// What the arms CANNOT witness: the RR slice expiry that produces the interleaving on
// target, which a returning arch_switch cannot reproduce. The action stands in for that thread.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class CapSweep : public KSeam
            {
            };

            // gtest runs a *DeathTest suite ahead of the others, which is the documented
            // placement for a forking case.
            class CapSweepDeathTest : public KSeam
            {
            };

            constexpr uint8_t PRIO_SWEEPER = 5;
            // Under every sweeper, so the wakes the sweeps issue are deferred by the dying
            // guard and the trace stays about the sweeps rather than about scheduling.
            constexpr uint8_t PRIO_PEER = 4;

            // Two chunks exactly: one live cap on each side of the boundary is what makes
            // resumption observable, and a sweep that fits in one chunk proves nothing.
            constexpr uint32_t SWEEP_WIDTH = KICKOS_CAP_FIRST_DYNAMIC + KCAP_TEARDOWN_CHUNK + 1;
            // gap 1 is the release cap_teardown opens before its first chunk, so gap 2 is the
            // boundary after it. A new IrqLock anywhere in the sweep renumbers these, and the
            // trace assertions are what say so -- do not renumber the expected string without
            // checking which gap the action now lands in.
            constexpr uint32_t GAP_AFTER_FIRST_CHUNK = 2;
            static_assert(KICKOS_CAP_FIRST_DYNAMIC < KCAP_TEARDOWN_CHUNK,
                          "the first dynamic slot must fall in the FIRST chunk, or the "
                          "ordinal above names a different boundary");

            Thread* g_inner = nullptr;
            Thread* g_closer = nullptr;
            uint32_t g_closer_cap = 0;

            void sweep_the_inner_thread()
            {
                trace_add("nest-in");
                cap_teardown(g_inner);
                trace_add("nest-out");
                // A COUNT, not a flag: a flag here would clear the outer sweep's in-flight
                // status too.
                if (cap_teardown_active())
                {
                    trace_add("outer-live");
                }
                else
                {
                    trace_add("outer-lost");
                }
            }

            void close_the_closers_cap()
            {
                trace_add("close");
                EXPECT_EQ(handle_close(g_closer, g_closer_cap), 0) << "the close was accepted";
            }

            // called directly rather than through exit_current: the subject is the sweep, and
            // an exit ends in a longjmp that may not cross a gap action.
            Thread* dying_sweeper(int slot, uint32_t width)
            {
                Thread* const c = spawn(slot, PRIO_SWEEPER);
                sched::reschedule();
                EXPECT_EQ(kernel().current, c) << "fixture: the sweeper is current";
                c->dying = true;
                attach_caps(c, width);
                return c;
            }

            // Plain is what makes the sender boost nothing, so its priority is the arm's to
            // choose.
            Thread* endpoint_cap_with_sender(Thread* owner, int index, int sender_slot)
            {
                Thread* const sender = spawn(sender_slot, PRIO_PEER);
                Endpoint* const ep = endpoint();
                int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
                endpoint_server_set(ep, owner);
                ep->recv_holders = 1;
                cap_install_at(owner, index, handle, CapType::CAP_ENDPOINT, CAP_WAIT);
                park_plain_sender(sender, ep);
                return sender;
            }

            uint32_t seat_a_closable_cap(Thread* t)
            {
                attach_caps(t, KICKOS_CAP_FIRST_DYNAMIC + 1);
                int sem_handle = 0;
                (void) semaphore(&sem_handle);
                uint32_t cap = 0;
                EXPECT_EQ(cap_install(t, sem_handle, CapType::CAP_SEM, CAP_WAIT, &cap), 0)
                    << "fixture: the closable cap was installed";
                return cap;
            }
        }

        // --- two sweeps at once --------------------------------------------------------

        // The arm the depth counter exists for. The inner sweep runs where a real one would
        // start -- between chunks, with the outer sweep holding no lock -- and the outer
        // sweep's own last chunk is what proves it resumed.
        TEST_F(CapSweep, a_second_sweep_nests_in_a_chunk_gap_and_the_first_resumes)
        {
            Thread* const outer = dying_sweeper(0, SWEEP_WIDTH);
            Thread* const sender = endpoint_cap_with_sender(outer, KICKOS_CAP_FIRST_DYNAMIC, 1);

            // The LAST slot, past the boundary: only a resumed sweep reaches it.
            Thread* const waiter = spawn(2, PRIO_PEER);
            int mtx_handle = 0;
            Mutex* const m = own_mutex(outer, &mtx_handle);
            cap_install_at(outer, static_cast<int>(SWEEP_WIDTH) - 1, mtx_handle,
                           CapType::CAP_MUTEX, CAP_WAIT);
            park_mutex_waiter(waiter, m);

            Thread* const inner = spawn(3, PRIO_SWEEPER);
            inner->dying = true;
            attach_caps(inner, KICKOS_CAP_FIRST_DYNAMIC + 1);
            Thread* const inner_sender =
                endpoint_cap_with_sender(inner, KICKOS_CAP_FIRST_DYNAMIC, 4);
            g_inner = inner;

            g_switches = 0;
            trace_reset();
            run_in_chunk_gap(sweep_the_inner_thread, GAP_AFTER_FIRST_CHUNK);

            cap_teardown(outer);

            EXPECT_STREQ(trace(), "gap1 gap2 nest-in nest-out outer-live gap3 gap4")
                << "the inner sweep runs between chunks and the outer one opens another";
            EXPECT_EQ(g_switches, 0u) << "no woken peer outranked either sweeper";
            EXPECT_EQ(sender->wait_result, -KOS_EPIPE) << "the outer sweep's first chunk drained";
            EXPECT_EQ(inner_sender->wait_result, -KOS_EPIPE) << "and the inner sweep did its own";
            EXPECT_EQ(m->owner, waiter) << "the cap past the boundary was released after the nest";
            EXPECT_EQ(outer->held_list, nullptr) << "the outer sweep completed its held list";
            EXPECT_FALSE(cap_teardown_active()) << "both sweeps balanced their depth";
        }

        // --- what a live sweep does to a concurrent voluntary close --------------------

        // handle_close reclaims the console for the caller, because a voluntary close has no
        // teardown loop to finish first. Not while a sweep is in flight: that sweeper may
        // still hold an IRQ cap on the line, and its own exit runs the sticky note instead.
        // The sweep here needs no live cap at all -- the depth is what the reader tests.
        TEST_F(CapSweep, a_close_in_a_chunk_gap_defers_the_console_reclaim)
        {
            Thread* const outer = dying_sweeper(0, SWEEP_WIDTH);
            g_closer = spawn(1, PRIO_PEER);
            g_closer_cap = seat_a_closable_cap(g_closer);

            trace_reset();
            run_in_chunk_gap(close_the_closers_cap, GAP_AFTER_FIRST_CHUNK);

            cap_teardown(outer);

            EXPECT_STREQ(trace(), "gap1 gap2 close gap3 gap4")
                << "the close lands between chunks, with the sweep still to finish";
            EXPECT_EQ(g_console_reclaimed, 0u)
                << "a close inside a live sweep leaves the reclaim to the sweeper";
            EXPECT_FALSE(cap_teardown_active()) << "the sweep balanced its depth";
        }

        // The control for the arm above, and it is what makes that one a claim about the
        // sweep rather than about handle_close never reclaiming.
        TEST_F(CapSweep, the_same_close_outside_a_sweep_reclaims_at_once)
        {
            g_closer = spawn(0, PRIO_PEER);
            g_closer_cap = seat_a_closable_cap(g_closer);

            trace_reset();
            close_the_closers_cap();

            EXPECT_STREQ(trace(), "close reclaim") << "the closer reclaims the console itself";
            EXPECT_EQ(g_console_reclaimed, 1u) << "exactly once";
        }

        // --- reset()'s guard against a leaked IrqLock -----------------------------------

        // note_irq_save with no matching restore stands in for an arm that leaked an
        // IrqLock. reset() must refuse: a silent zeroing here would shut note_irq_restore's
        // gap gate for every later arm's run_in_chunk_gap action.
        TEST_F(CapSweepDeathTest, a_leaked_irq_lock_refuses_the_next_reset)
        {
            note_irq_save();

            KICKOS_EXPECT_FIXTURE_REFUSAL(reset(), "an IrqLock leaked");

            note_irq_restore();
        }
    }
}
