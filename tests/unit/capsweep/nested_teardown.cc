// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// cap_teardown drops IrqLock between chunks, so a second dying thread scheduled into that gap
// starts a sweep of its own.
//
// The claim these arms make is an ORDER; a counter oracle cannot fail on a reordering.
//
// On target the interleaving comes from RR slice expiry; here arch_switch returns, so a gap
// action stands in for that second thread.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
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

            // The *DeathTest suffix is what makes gtest run this suite ahead of the others.
            class CapSweepDeathTest : public KSeam
            {
            };

            constexpr uint8_t PRIO_SWEEPER = 5;
            // Under every sweeper, so the wakes the sweeps issue are deferred by the dying
            // guard and the trace stays about the sweeps rather than about scheduling.
            constexpr uint8_t PRIO_PEER = 4;
            // The voluntary closer, and the sender its EPIPE releases. The closer must
            // outrank the publisher and be outranked by the sender, or the switch the
            // console arm dates never happens.
            constexpr uint8_t PRIO_CLOSER = 5;
            constexpr uint8_t PRIO_ABOVE_CLOSER = 6;
            static_assert(PRIO_PEER < PRIO_CLOSER and PRIO_CLOSER < PRIO_ABOVE_CLOSER,
                          "closer between the publisher and the sender");

            // Two chunks exactly: one live cap on each side of the boundary is what makes
            // resumption observable, and a sweep that fits in one chunk proves nothing.
            constexpr uint32_t SWEEP_WIDTH = KICKOS_CAP_FIRST_DYNAMIC + KCAP_TEARDOWN_CHUNK + 1;
            // gap 1 is the release cap_teardown opens before its first chunk, so gap 2 is the
            // boundary after it. A new IrqLock anywhere in the sweep renumbers these, and the
            // trace assertions are what say so. Do not renumber the expected string without
            // checking which gap the action now lands in.
            constexpr uint32_t GAP_AFTER_FIRST_CHUNK = 2;
            constexpr uint32_t GAP_BEFORE_FIRST_CHUNK = 1;
            static_assert(KICKOS_CAP_FIRST_DYNAMIC < KCAP_TEARDOWN_CHUNK,
                          "the first dynamic slot must fall in the FIRST chunk, or the "
                          "ordinal above names a different boundary");

            // Any in-range line does: what a claim reads is the dispatch slot, and no arm
            // here ever arms it.
            constexpr int CONSOLE_LINE = 14;
            // The dying thread's line cap must land in a LATER chunk than the endpoint cap
            // whose EPIPE releases the peer. Below the boundary both fall in one masked
            // window and no ordering is observable at all.
            constexpr uint32_t IRQ_CAP_INDEX = KCAP_TEARDOWN_CHUNK;
            static_assert(IRQ_CAP_INDEX < SWEEP_WIDTH,
                          "the line cap must still fit the sweeper's table");

            Thread* g_inner = nullptr;
            Thread* g_closer = nullptr;
            uint32_t g_closer_cap = 0;
            Thread* g_supervisor = nullptr;
            int g_claim_rc = 0;
            Thread* g_line_owner = nullptr;
            uint8_t g_line_slot_type = 0xFFu;

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

            // Stands in for the SUPERVISOR the sweep's own EPIPE released: on target it is a
            // thread above the dying driver, which is what makes it run before the sweep
            // finishes; here the gap action is that scheduling.
            void supervisor_claims_the_line()
            {
                trace_add("claim");
                uint32_t cap = 0;
                g_claim_rc = irq_claim(g_supervisor, CONSOLE_LINE, 0, &cap);
            }

            // Reads the line cap's own slot as well as claiming the line: the slot is what the
            // pre-pass empties, and a claim alone would also answer 0 for a line the sweep had
            // detached while leaving the entry live.
            void inspect_the_line_before_the_first_chunk()
            {
                trace_add("look");
                g_line_slot_type = cap_slot(g_line_owner->caps, IRQ_CAP_INDEX)->type;
                uint32_t cap = 0;
                g_claim_rc = irq_claim(g_supervisor, CONSOLE_LINE, 0, &cap);
            }

            // Called directly rather than through exit_current: the subject is the sweep, and
            // an exit ends in a longjmp that may not cross a gap action.
            Thread* dying_sweeper(int slot, uint32_t width)
            {
                Thread* const c = spawn(slot, PRIO_SWEEPER);
                sched::reschedule();
                EXPECT_EQ(kernel().current[arch_cpu_id()], c) << "fixture: the sweeper is current";
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

            // The published console, served by `owner`. A publisher of its own, so `owner`
            // holds exactly ONE cap on the endpoint and the sweep's recv_holders arithmetic is
            // the arm's subject rather than the seat in slot 0.
            int publish_console_served_by(Thread* owner, int index, int publisher_slot)
            {
                Thread* const publisher = spawn(publisher_slot, PRIO_PEER);
                attach_caps(publisher, KICKOS_CAP_FIRST_DYNAMIC + 1);
                Endpoint* const ep = endpoint();
                int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
                EXPECT_TRUE(cap_console_publish(publisher, handle))
                    << "fixture: the console endpoint was published";
                cap_install_at(owner, index, handle, CapType::CAP_ENDPOINT, CAP_WAIT);
                // Through the real counter locator, so recv_holders and endpoint_refs move
                // together: a hand-written 1 in recv_holders would make the sweep's drop the
                // last reference and take a leak-never-strand branch instead of this arm.
                EXPECT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, handle, CAP_WAIT))
                    << "fixture: the receiver cap took its references";
                return handle;
            }

            // The real irq_claim mints into the FIRST free slot, so the slots below the chunk
            // boundary are taken out of the free list to place the cap where the sweep reaches
            // it only after a gap. An unlinked slot stays EMPTY, which the sweep skips.
            void claim_the_line_past_the_boundary(Thread* owner)
            {
                for (uint32_t i = KICKOS_CAP_FIRST_DYNAMIC; i < IRQ_CAP_INDEX; i++)
                {
                    if (cap_slot(owner->caps, i)->type
                        == static_cast<uint8_t>(CapType::CAP_EMPTY))
                    {
                        cap_run_free_unlink(owner->caps, i, &owner->cap_free_head);
                    }
                }
                uint32_t cap = 0;
                EXPECT_EQ(irq_claim(owner, CONSOLE_LINE, 0, &cap), 0)
                    << "fixture: the dying thread owns the line";
                EXPECT_EQ(cap & KCAP_INDEX_MASK, IRQ_CAP_INDEX)
                    << "fixture: the line cap landed past the first chunk";
            }

            // The handle userspace would name for a cap the fixture seated by INDEX.
            uint32_t cap_handle_at(Thread* t, uint32_t index)
            {
                return (static_cast<uint32_t>(cap_slot(t->caps, index)->gen) << KCAP_INDEX_BITS)
                       | index;
            }

            // The gap tokens come from the watch, not from an action: an arm that only needs
            // to DATE events against the chunk boundaries arms it with no interleaving.
            void watch_chunk_gaps()
            {
                run_in_chunk_gap(nullptr, 0);
            }
        }

        // --- two sweeps at once --------------------------------------------------------

        // The arm the depth counter exists for. The inner sweep runs where a real one would
        // start, between chunks with the outer sweep holding no lock, and the outer
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

        // --- the line a peer respawns into ---------------------------------------------

        // The name-keyed half: an IRQ line is named by NUMBER, so until the dying thread's
        // binding is detached the same line answers -KOS_EBUSY. The sweep's own endpoint arm
        // releases a parked supervisor, and on target that supervisor outranks its driver and
        // runs at the very next boundary, before a line cap seated past it is swept.
        TEST_F(CapSweep, a_peer_the_sweep_wakes_can_claim_the_dying_threads_line)
        {
            Thread* const outer = dying_sweeper(0, SWEEP_WIDTH);
            Thread* const sender = endpoint_cap_with_sender(outer, KICKOS_CAP_FIRST_DYNAMIC, 1);
            claim_the_line_past_the_boundary(outer);

            g_supervisor = spawn(2, PRIO_PEER);
            attach_caps(g_supervisor, KICKOS_CAP_FIRST_DYNAMIC + 1);
            g_claim_rc = -1;

            trace_reset();
            run_in_chunk_gap(supervisor_claims_the_line, GAP_AFTER_FIRST_CHUNK);

            cap_teardown(outer);

            EXPECT_STREQ(trace(), "gap1 gap2 claim gap3 gap4")
                << "the claim lands between chunks, with the line cap still to be swept";
            EXPECT_EQ(sender->wait_result, -KOS_EPIPE)
                << "and it is a peer THIS sweep released, not an unrelated thread";
            EXPECT_EQ(g_claim_rc, 0) << "the line was already detached when the peer asked";
            EXPECT_FALSE(cap_teardown_active()) << "the sweep balanced its depth";
        }

        // The pre-pass is gated on a COUNT, so the arm that says the gate never skips a pass
        // that is owed has to date the release against the first gap of all, not against the
        // boundary the line cap's own chunk would reach anyway. A thread holding one line is
        // exactly the case the count does not let through.
        TEST_F(CapSweep, a_held_line_is_released_before_the_sweep_opens_any_gap)
        {
            Thread* const outer = dying_sweeper(0, SWEEP_WIDTH);
            claim_the_line_past_the_boundary(outer);
            g_line_owner = outer;

            g_supervisor = spawn(1, PRIO_PEER);
            attach_caps(g_supervisor, KICKOS_CAP_FIRST_DYNAMIC + 1);
            g_claim_rc = -1;
            g_line_slot_type = 0xFFu;

            trace_reset();
            run_in_chunk_gap(inspect_the_line_before_the_first_chunk, GAP_BEFORE_FIRST_CHUNK);

            cap_teardown(outer);

            EXPECT_STREQ(trace(), "gap1 look gap2 gap3 gap4")
                << "the look lands in the sweep's very first gap";
            EXPECT_EQ(g_line_slot_type, static_cast<uint8_t>(CapType::CAP_EMPTY))
                << "the line cap's slot was already emptied, chunks away from its own boundary";
            EXPECT_EQ(g_claim_rc, 0) << "and the line itself was detached, not merely unnamed";
        }

        // --- the console the same wake exposes ------------------------------------------

        // The other name-keyed release, and the reason the pass above must precede the loop:
        // the reclaim runs at the NOTE, in the masked window that EPIPEs the peer, so no peer
        // can observe a console the sweep has already decided is dead. Dated by the trace,
        // which is the only oracle that can fail on this being moved after the loop.
        TEST_F(CapSweep, the_endpoint_arm_reclaims_the_console_before_it_drops_the_lock)
        {
            Thread* const outer = dying_sweeper(0, SWEEP_WIDTH);
            Thread* const sender = spawn(1, PRIO_PEER);
            int const handle = publish_console_served_by(outer, KICKOS_CAP_FIRST_DYNAMIC, 2);
            park_plain_sender(sender, kernel().endpoints.resolve(handle));

            trace_reset();
            watch_chunk_gaps();

            cap_teardown(outer);

            EXPECT_STREQ(trace(), "gap1 note reclaim gap2 gap3 gap4")
                << "note and reclaim both land inside the chunk that EPIPEd the sender";
            EXPECT_EQ(sender->wait_result, -KOS_EPIPE) << "the sender was released by that arm";
            EXPECT_EQ(g_console_noted, 1u) << "the published endpoint lost its last receiver";
            EXPECT_EQ(g_console_reclaimed, 1u) << "exactly once";
        }

        // The control: the same sweep over an UNPUBLISHED endpoint decides nothing about the
        // console, so the arm above cannot pass on a reclaim that fires for any dying thread.
        TEST_F(CapSweep, a_sweep_over_an_unpublished_endpoint_reclaims_nothing)
        {
            Thread* const outer = dying_sweeper(0, SWEEP_WIDTH);
            Thread* const sender = endpoint_cap_with_sender(outer, KICKOS_CAP_FIRST_DYNAMIC, 1);

            trace_reset();
            watch_chunk_gaps();

            cap_teardown(outer);

            EXPECT_EQ(sender->wait_result, -KOS_EPIPE) << "the endpoint arm still ran";
            EXPECT_EQ(g_console_noted, 0u) << "nothing was published, so nothing was noted";
            EXPECT_EQ(g_console_reclaimed, 0u) << "and nothing was reclaimed";
        }

        // --- what a live sweep does to a concurrent voluntary close --------------------

        // A close that takes the published endpoint's last receiver away reclaims for the
        // CALLER, at the same site the sweep uses, and a sweep in flight elsewhere does not
        // postpone it: a counted sweep has already released every line it held.
        TEST_F(CapSweep, a_close_in_a_chunk_gap_reclaims_the_console_at_once)
        {
            Thread* const outer = dying_sweeper(0, SWEEP_WIDTH);
            g_closer = spawn(1, PRIO_PEER);
            attach_caps(g_closer, KICKOS_CAP_FIRST_DYNAMIC + 1);
            (void) publish_console_served_by(g_closer, KICKOS_CAP_FIRST_DYNAMIC, 2);
            g_closer_cap = cap_handle_at(g_closer, KICKOS_CAP_FIRST_DYNAMIC);

            trace_reset();
            run_in_chunk_gap(close_the_closers_cap, GAP_AFTER_FIRST_CHUNK);

            cap_teardown(outer);

            EXPECT_STREQ(trace(), "gap1 gap2 close note reclaim gap3 gap4")
                << "the close lands between chunks and decides the console there";
            EXPECT_EQ(g_console_reclaimed, 1u)
                << "a live sweep elsewhere does not postpone the closer's reclaim";
            EXPECT_FALSE(cap_teardown_active()) << "the sweep balanced its depth";
        }

        // The control for the arm above: the same close with no sweep in flight, which is what
        // makes that one a claim about the sweep rather than about the close site itself.
        TEST_F(CapSweep, the_same_close_outside_a_sweep_reclaims_at_once)
        {
            g_closer = spawn(0, PRIO_PEER);
            attach_caps(g_closer, KICKOS_CAP_FIRST_DYNAMIC + 1);
            (void) publish_console_served_by(g_closer, KICKOS_CAP_FIRST_DYNAMIC, 1);
            g_closer_cap = cap_handle_at(g_closer, KICKOS_CAP_FIRST_DYNAMIC);

            trace_reset();
            close_the_closers_cap();

            EXPECT_STREQ(trace(), "close note reclaim")
                << "the closer notes and reclaims the console itself";
            EXPECT_EQ(g_console_reclaimed, 1u) << "exactly once";
        }

        // The switch the close ADMITS, which no arm above can show: their closer is either
        // dying or under every peer, so wake declines and the EPIPE loop is silent. Here
        // the closer is alive and the released sender outranks it, so the wake reaches
        // arch_switch, which swaps INLINE on the sim. The trace is the only oracle that
        // fails when the console decision moves after the wake.
        TEST_F(CapSweep, a_voluntary_close_reclaims_before_the_wake_it_admits)
        {
            g_closer = spawn(0, PRIO_CLOSER);
            attach_caps(g_closer, KICKOS_CAP_FIRST_DYNAMIC + 1);
            int const handle = publish_console_served_by(g_closer, KICKOS_CAP_FIRST_DYNAMIC, 1);
            g_closer_cap = cap_handle_at(g_closer, KICKOS_CAP_FIRST_DYNAMIC);
            Thread* const sender = spawn(2, PRIO_ABOVE_CLOSER);
            park_plain_sender(sender, kernel().endpoints.resolve(handle));

            sched::reschedule();
            EXPECT_EQ(kernel().current[arch_cpu_id()], g_closer) << "fixture: the closer holds the CPU";
            EXPECT_FALSE(g_closer->dying) << "fixture: a voluntary close, not a teardown";

            trace_reset();
            close_the_closers_cap();

            EXPECT_STREQ(trace(), "close note reclaim switch1>3")
                << "the console is decided before the sender the same call releases can run";
            EXPECT_EQ(sender->wait_result, -KOS_EPIPE)
                << "and it is a sender THIS close released, not an unrelated thread";
            EXPECT_EQ(g_console_noted, 1u) << "the published endpoint lost its last receiver";
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
