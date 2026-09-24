// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test IRQ dispatch against concurrent teardown using two host threads as cores.
// QEMU GIC routing serializes these operations on one core.
// Single-threaded cases invoke teardown inside a simulated peer handler.
// Threaded cases block the peer on the kernel lock while teardown runs;
// doorbells can be answered there without completing the dispatch epoch.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "irq_seam.h"

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irq_route.h>
#include <kickos/notify.h>
#include <kickos/irqlock.h>
#include <kickos/sys/errno.h>
#include <kickos/thread.h>

using kickos::irqfix::OP_IPI_SEND;
using kickos::irqfix::OP_IPI_WAIT;
using kickos::irqfix::OP_MASK;

namespace
{
    // Two kernel cores, so the peer of core zero is core one and a rendezvous mask is 0b10.
    static_assert(KICKOS_KERNEL_CORES == 2,
                  "these arms speak as core one against core zero; a different count makes "
                  "every peer-mask assertion below describe another machine");

    constexpr int LINE_TARGET = 5;  // the line whose binding the arms tear down
    constexpr int LINE_CARRIER = 7; // the line whose handler the peer is inside
    constexpr int LINE_FREE = 9;    // a line with no driver, for the rebind arm

    // How long a threaded arm waits for its peer before declaring the hand-off broken; kept
    // well under the ctest timeout so a blown budget fails with a verdict instead of hanging.
    constexpr auto HANDOFF_BUDGET = std::chrono::seconds(10);

    void probe_handler(void* arg)
    {
        kickos::irqfix::g_probe_calls++;
        kickos::irqfix::g_probe_arg = arg;
    }

    // The peer's handler: whatever the arm armed runs here, inside the dispatch entry, with
    // this core's epoch raised.
    void carrier_handler(void*)
    {
        void (*action)() = kickos::irqfix::g_probe_action;
        kickos::irqfix::g_probe_action = nullptr;
        if (action != nullptr)
        {
            action();
        }
    }

    // Spin on `pred` up to the hand-off budget. False means the budget blew.
    template <typename Pred>
    bool wait_for(Pred pred)
    {
        auto const deadline = std::chrono::steady_clock::now() + HANDOFF_BUDGET;
        while (not pred())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

    int index_of(int obj_handle)
    {
        kickos::Kernel& k = kickos::kernel();
        return k.irq_bindings.index_of(k.irq_bindings.resolve(obj_handle));
    }

    // Static storage, never reassigned: Thread holds an Atomic and so is not assignable, and
    // irq_claim reads nothing of it but its address and its mask.
    kickos::Thread g_claim_thread;

    // A line is claimed only by a thread pinned to the core it claims from.
    void pin_claimer(uint32_t core)
    {
        g_claim_thread.affinity = 1u << core;
    }

    struct IrqQuiesce : public ::testing::Test
    {
        int handle = -1;
        int index = -1;

        void SetUp() override
        {
            kickos::irqfix::reset();
            kickos::irqfix::reset_lock();
            kickos::irqfix::reset_kernel();
            kickos::irqfix::reset_caps();
            kickos::irqfix::g_core = 0;
            pin_claimer(0);
            ASSERT_TRUE(kickos::irq_attach(LINE_CARRIER, carrier_handler, nullptr));
        }

        // A tier-1 binding on LINE_TARGET with one reference, installed the way irq_claim
        // installs one: the ISR's argument is the slot's address.
        void seat_binding()
        {
            kickos::Kernel& k = kickos::kernel();
            index = k.irq_bindings.alloc();
            ASSERT_GE(index, 0);
            handle = k.irq_bindings.handle_for(index);
            kickos::IrqBinding* b = k.irq_bindings.at(index);
            ASSERT_NE(b, nullptr);
            *b = kickos::IrqBinding();
            b->line = LINE_TARGET;
            k.irq_refs[index] = 1;
            ASSERT_TRUE(kickos::irq_attach(LINE_TARGET, probe_handler, b));
        }

        // Enter the dispatch entry as core one, running `action` from inside it as core zero.
        static void peer_dispatch_running(void (*action)())
        {
            kickos::irqfix::g_probe_action = action;
            kickos::irqfix::g_core = 1;
            kickos_isr_irq(LINE_CARRIER);
            kickos::irqfix::g_core = 0;
            ASSERT_EQ(kickos::irqfix::g_probe_action, nullptr)
                << "the carrier handler never ran, so no arm below observed a peer inside the "
                   "dispatch entry";
        }
    };

    // ---------------------------------------------------------------------------------------
    // The positive control, first: the entry still dispatches the pair it was given. Without
    // this every arm below could be a dispatch that stopped working.
    TEST_F(IrqQuiesce, DispatchDeliversThePublishedPair)
    {
        seat_binding();
        kickos::IrqBinding* b = kickos::kernel().irq_bindings.at(index);
        ASSERT_NE(b, nullptr);

        kickos_isr_irq(LINE_TARGET);
        EXPECT_EQ(kickos::irqfix::g_probe_calls, 1u);
        EXPECT_EQ(kickos::irqfix::g_probe_arg, static_cast<void*>(b));
    }

    // The pair a dispatch would run is the whole pair that was published, never a mix of two
    // bindings, and the default's argument is the line rather than a record's.
    TEST_F(IrqQuiesce, PublishedPairIsWholeOrAbsent)
    {
        seat_binding();
        kickos::IrqBinding* b = kickos::kernel().irq_bindings.at(index);
        ASSERT_NE(b, nullptr);

        kickos::IrqDispatch const bound = kickos::irq_published(LINE_TARGET);
        EXPECT_EQ(bound.handler, static_cast<kickos::IrqHandler>(probe_handler));
        EXPECT_EQ(bound.arg, static_cast<void*>(b));

        kickos::irq_detach(LINE_TARGET);
        kickos::IrqDispatch const freed = kickos::irq_published(LINE_TARGET);
        EXPECT_NE(freed.handler, static_cast<kickos::IrqHandler>(probe_handler))
            << "the line still names the detached driver's record";
        EXPECT_EQ(freed.arg, reinterpret_cast<void*>(static_cast<intptr_t>(LINE_TARGET)))
            << "the null-object default must be handed the line it was entered for";
    }

    TEST_F(IrqQuiesce, TeardownWithNoPeerInsideFreesTheSlot)
    {
        seat_binding();

        kickos::irq_ref_drop(handle, false);

        EXPECT_EQ(kickos::kernel().irq_bindings.resolve(handle), nullptr)
            << "the slot was not freed with no peer inside the dispatch entry";
        EXPECT_NE(kickos::irq_published(LINE_TARGET).handler,
                  static_cast<kickos::IrqHandler>(probe_handler))
            << "the line was not put back on the null-object default";
    }

    // Nothing is poked, on any path: the retirement decides by reading epochs, so a teardown
    // that sends a doorbell is a teardown waiting for an answer.
    TEST_F(IrqQuiesce, TeardownPokesNobody)
    {
        seat_binding();
        kickos::irqfix::reset();
        kickos::irqfix::g_core = 0;

        kickos::irq_ref_drop(handle, false);

        EXPECT_EQ(kickos::irqfix::ipi_sends(0), 0u)
            << "the teardown rendezvoused with a peer, so it can be made to wait under the "
               "kernel lock that a peer's own handler needs to finish";
        EXPECT_EQ(kickos::irqfix::count_of(OP_IPI_WAIT), 0u);
    }

    TEST_F(IrqQuiesce, TeardownMasksTheLineItReleases)
    {
        seat_binding();
        kickos::irqfix::reset();
        kickos::irqfix::g_core = 0;

        kickos::irq_ref_drop(handle, false);

        int const first_mask = kickos::irqfix::first_of(OP_MASK);
        ASSERT_GE(first_mask, 0) << "the teardown masked no line at all";
        EXPECT_EQ(kickos::irqfix::event(static_cast<unsigned>(first_mask)).arg,
                  static_cast<uint32_t>(LINE_TARGET));
    }

    // ---------------------------------------------------------------------------------------
    // A rebind is admitted with a peer inside the entry: one word publishes the pair, so a
    // torn read cannot keep anybody out.
    bool g_arm_attached = false;

    void attach_from_core_zero()
    {
        kickos::irqfix::g_core = 0;
        g_arm_attached = kickos::irq_attach(LINE_FREE, probe_handler,
                                           reinterpret_cast<void*>(0xABCDu));
        kickos::irqfix::g_core = 1;
    }

    TEST_F(IrqQuiesce, RebindUnderPeerDispatchTakes)
    {
        g_arm_attached = false;

        peer_dispatch_running(attach_from_core_zero);

        EXPECT_TRUE(g_arm_attached)
            << "the rebind was refused while core one was inside the dispatch entry, which is "
               "the refusal this mechanism exists to remove";
        EXPECT_EQ(kickos::irq_published(LINE_FREE).handler,
                  static_cast<kickos::IrqHandler>(probe_handler));
        EXPECT_EQ(kickos::irq_published(LINE_FREE).arg, reinterpret_cast<void*>(0xABCDu));
    }

    int g_claim_rc = 0;
    uint32_t g_claim_cap = 0;

    void claim_from_core_zero()
    {
        kickos::irqfix::g_core = 0;
        g_claim_rc = kickos::irq_claim(&g_claim_thread, LINE_FREE, 0u, &g_claim_cap);
        kickos::irqfix::g_core = 1;
    }

    TEST_F(IrqQuiesce, ClaimUnderPeerDispatchTakesAndKeepsItsCap)
    {
        g_claim_rc = -1;
        g_claim_cap = 0;

        peer_dispatch_running(claim_from_core_zero);

        EXPECT_EQ(g_claim_rc, 0)
            << "the claim was refused while a peer was inside the dispatch entry";
        EXPECT_EQ(kickos::irqfix::closes(), 0u)
            << "the claim undid the capability for a line it did in fact bind";
        EXPECT_NE(kickos::irq_published(LINE_FREE).arg, nullptr);
    }

    // =======================================================================================
    // The threaded arms. Core one runs on its own thread and stays inside the dispatch entry
    // of the very line being torn down, its sem_post blocked on the kernel lock core zero
    // holds. Every observation core zero makes below is made while that is true.

    struct Cycle
    {
        std::atomic<bool> lock_held{false};   // core zero holds the kernel lock
        std::atomic<bool> peer_done{false};   // core one left the dispatch entry
        std::atomic<bool> handoff_ok{true};   // every hand-off met its budget
        // Taken by core zero with the peer still wedged.
        std::atomic<bool> slot_live_at_return{false};
        std::atomic<unsigned> refs_at_return{0};
        std::atomic<unsigned> sends_in_teardown{0};
        std::atomic<bool> teardown_returned{false};
    };
    Cycle g_cyc;

    // Core one: wait for core zero to own the lock, then enter the dispatch entry and wedge.
    void peer_core_one(int line)
    {
        kickos::irqfix::g_core = 1;
        if (not wait_for([] { return g_cyc.lock_held.load(); }))
        {
            g_cyc.handoff_ok.store(false);
            return;
        }
        kickos_isr_irq(line); // irq_event_isr -> notify_post -> IrqLock -> blocked on core zero
        g_cyc.peer_done.store(true);
    }

    struct IrqCycle : public IrqQuiesce
    {
        int obj = -1;
        int idx = -1;
        uint32_t cap = 0;
        // The peer dispatch holds this stable pool address. With no bound thread on the
        // object it signals, completed delivery leaves the bit pending there.
        kickos::IrqBinding* target = nullptr;

        // The bit the target raises: irq_bind_notify copies the capability's badge, and an
        // unbadged one is bit 0.
        bool target_posted() const
        {
            return target->notify != nullptr and (target->notify->pending & 1u) != 0u;
        }

        void SetUp() override
        {
            IrqQuiesce::SetUp();
            g_cyc.lock_held.store(false);
            g_cyc.peer_done.store(false);
            g_cyc.handoff_ok.store(true);
            g_cyc.slot_live_at_return.store(false);
            g_cyc.refs_at_return.store(0);
            g_cyc.sends_in_teardown.store(0);
            g_cyc.teardown_returned.store(false);

            // The production route: irq_claim is what binds irq_event_isr, and only that
            // handler reaches sem_post and so the lock. A hand-installed probe handler would
            // never block, and the arm would prove nothing.
            //
            // Claimed on core one, the core the peer dispatches on: irq_event_isr drops a raise
            // taken off its claim core before it reaches the lock.
            kickos::irqfix::g_core = 1;
            pin_claimer(1);
            int const claimed = kickos::irq_claim(&g_claim_thread, LINE_TARGET, 0u, &cap);
            pin_claimer(0);
            kickos::irqfix::g_core = 0;
            ASSERT_EQ(claimed, 0);
            obj = kickos::irqfix::installed_handle();
            ASSERT_GE(obj, 0);
            idx = index_of(obj);
            ASSERT_GE(idx, 0);
            target = kickos::kernel().irq_bindings.at(idx);
            ASSERT_NE(target, nullptr);
            ASSERT_EQ(kickos::kernel().irq_refs[idx], 1u);
            // The line needs somewhere to raise: without it the ISR masks and returns, and
            // every "the post completed" assertion below would read as a failure. The seam's
            // one object, reached through the real attach path.
            kickos::irqfix::the_notification()->pending = 0;
            ASSERT_EQ(kickos::irq_bind_notify(&g_claim_thread, cap, 0u), 0);
        }

        // Run the teardown as core zero with core one wedged inside the dispatch entry.
        void tear_down_against_wedged_peer()
        {
            std::thread peer(peer_core_one, LINE_TARGET);
            {
                kickos::IrqLock lock; // core zero takes the kernel lock
                g_cyc.lock_held.store(true);
                // Not merely "the peer started": wait until it has actually failed to take the
                // lock, which is the peer inside the entry with its post blocked.
                if (not wait_for([] { return kickos::irqfix::g_lock_blocked.load(); }))
                {
                    g_cyc.handoff_ok.store(false);
                }
                else
                {
                    unsigned const before = kickos::irqfix::ipi_sends(0);
                    kickos::handle_close(&g_claim_thread, cap);
                    g_cyc.sends_in_teardown.store(kickos::irqfix::ipi_sends(0) - before);
                    g_cyc.slot_live_at_return.store(
                        kickos::kernel().irq_bindings.resolve(obj) != nullptr);
                    g_cyc.refs_at_return.store(kickos::kernel().irq_refs[idx]);
                    g_cyc.teardown_returned.store(true);
                }
            } // the lock drops here, which is what lets the peer's post through
            peer.join();
        }
    };

    // Claim one: the teardown does not wait. It runs to completion under the lock while the
    // peer needs that same lock to finish, so it may poke nobody and spin on nothing.
    TEST_F(IrqCycle, TeardownReturnsUnderAWedgedPeerWithoutWaiting)
    {
        tear_down_against_wedged_peer();

        ASSERT_TRUE(g_cyc.handoff_ok.load())
            << "the hand-off never happened: the peer did not reach the kernel lock from inside "
               "the dispatch entry, so this arm did not reproduce the cycle";
        EXPECT_TRUE(g_cyc.teardown_returned.load());
        EXPECT_EQ(g_cyc.sends_in_teardown.load(), 0u)
            << "the teardown rendezvoused with the peer it was blocking. The peer answers every "
               "doorbell from the acquire loop and can never lower its own epoch, so a teardown "
               "that reads a peer's cell refuses for as many rounds as it is given";
        EXPECT_TRUE(g_cyc.peer_done.load())
            << "the peer never left the dispatch entry, so the teardown did not release the "
               "lock its post needs";
        EXPECT_TRUE(target_posted()) << "the peer's post never completed";
    }

    // Claim two: the binding is not freed while a dispatch that could observe it is in flight.
    // The peer holds this slot's address as its pre-bound argument for the whole of the window
    // core zero makes this observation in.
    TEST_F(IrqCycle, RetiredBindingIsNotFreedUnderAnInFlightDispatch)
    {
        tear_down_against_wedged_peer();

        ASSERT_TRUE(g_cyc.handoff_ok.load()) << "the cycle was not reproduced";
        EXPECT_TRUE(g_cyc.slot_live_at_return.load())
            << "the slot went back to the pool while core one was inside the dispatch entry "
               "holding its address: the next delivery of a recycled slot writes another "
               "driver's semaphore";
    }

    // Claim three, availability: the line and the slot both come back once the peer leaves. A
    // refusal that never lifts is a permanent loss, and the reference must never be put back
    // with no capability left to drop it.
    TEST_F(IrqCycle, LineAndSlotAreRecoveredAfterThePeerLeaves)
    {
        tear_down_against_wedged_peer();

        ASSERT_TRUE(g_cyc.handoff_ok.load()) << "the cycle was not reproduced";
        ASSERT_TRUE(g_cyc.peer_done.load());

        EXPECT_EQ(g_cyc.refs_at_return.load(), 0u)
            << "the teardown put the reference back after the capability slot was already "
               "emptied, so nothing names this binding and nothing will ever drop it again";

        // The peer is out. Any later operation on the line drains what the retirement owes.
        uint32_t again = 0;
        int const rc = kickos::irq_claim(&g_claim_thread, LINE_TARGET, 0u, &again);

        EXPECT_EQ(rc, 0)
            << "the line is still bound to the torn-down driver, so it is lost for the lifetime "
               "of the image: a driver can turn load into permanent exhaustion by closing its "
               "last capability while its line is under delivery";
        EXPECT_EQ(kickos::kernel().irq_bindings.resolve(obj), nullptr)
            << "the retired slot never returned to the pool";
    }

    // A nested dispatch entry on the same core does not report that core out of the outer entry
    // it has not left. The epoch turns over on the nesting depth's zero crossings only.
    //
    // The teardown runs from inside the nested handler, not after it: a nested entry and exit
    // turn the parity over twice and put it back, so an observation taken after the inner
    // dispatch returned cannot see the hazard at all.
    uint32_t g_nest_cap = 0;
    int g_nest_obj = -1;
    bool g_nest_slot_live = false;
    void (*g_nested_action)() = nullptr;

    void nested_handler(void*)
    {
        void (*action)() = g_nested_action;
        g_nested_action = nullptr;
        if (action != nullptr)
        {
            action();
        }
    }

    // Runs as core zero from inside core one's inner dispatch entry.
    void drop_from_inside_the_nested_entry()
    {
        kickos::irqfix::g_core = 0;
        kickos::handle_close(&g_claim_thread, g_nest_cap);
        g_nest_slot_live = kickos::kernel().irq_bindings.resolve(g_nest_obj) != nullptr;
        kickos::irqfix::g_core = 1;
    }

    void enter_nested_entry()
    {
        g_nested_action = drop_from_inside_the_nested_entry;
        kickos_isr_irq(LINE_FREE); // a second entry on this same core, inside the first
    }

    TEST_F(IrqCycle, NestedEntryDoesNotReportTheCoreOutOfTheOuterOne)
    {
        ASSERT_TRUE(kickos::irq_attach(LINE_FREE, nested_handler, nullptr));
        g_nest_cap = cap;
        g_nest_obj = obj;
        g_nest_slot_live = false;
        g_nested_action = nullptr;

        peer_dispatch_running(enter_nested_entry);

        ASSERT_EQ(g_nested_action, nullptr)
            << "the nested handler never ran, so this arm observed no nested entry at all";
        EXPECT_TRUE(g_nest_slot_live)
            << "the slot was reclaimed while core one was still inside the OUTER dispatch entry: "
               "a nested entry turned its epoch over and reported the core quiescent";

        // And it does come back once that outer entry is left.
        uint32_t again = 0;
        EXPECT_EQ(kickos::irq_claim(&g_claim_thread, LINE_TARGET, 0u, &again), 0);
        EXPECT_EQ(kickos::kernel().irq_bindings.resolve(obj), nullptr);
    }

    // The grace period is a predicate, not a spin: with the peer already out, the very next
    // teardown reclaims inside its own call.
    TEST_F(IrqCycle, ReclamationNeedsNoSecondEventOnceThePeerIsOut)
    {
        tear_down_against_wedged_peer();
        ASSERT_TRUE(g_cyc.handoff_ok.load()) << "the cycle was not reproduced";
        ASSERT_TRUE(g_cyc.peer_done.load());

        uint32_t again = 0;
        ASSERT_EQ(kickos::irq_claim(&g_claim_thread, LINE_TARGET, 0u, &again), 0);
        int const second = kickos::irqfix::installed_handle();
        ASSERT_GE(second, 0);

        kickos::irqfix::g_core = 0;
        kickos::handle_close(&g_claim_thread, again);

        EXPECT_EQ(kickos::kernel().irq_bindings.resolve(second), nullptr)
            << "no core was inside the dispatch entry, so this teardown had nothing to outlive "
               "and had to reclaim within its own call";
    }
    // =======================================================================================
    // What a refused claim costs. A claim takes three resources in order: a publication
    // record, a binding slot, then a capability. Only the record can run out after the line
    // has been found free. The release path a refusal unwinds through reads the line to find the
    // record that owes the binding, so a refusal on a line naming no record has nowhere to put
    // the slot back.

    // Binding slots the pool will still hand out. Taken by draining it and putting every slot
    // straight back, which is a count and so is valid with bindings live: a live slot is absent
    // from both readings.
    unsigned free_bindings()
    {
        kickos::Kernel& k = kickos::kernel();
        int taken[KICKOS_MAX_IRQ_HANDLES];
        unsigned n = 0;
        while (n < KICKOS_MAX_IRQ_HANDLES)
        {
            int const i = k.irq_bindings.alloc();
            if (i < 0)
            {
                break;
            }
            taken[n] = k.irq_bindings.handle_for(i);
            n++;
        }
        for (unsigned j = 0; j < n; j++)
        {
            k.irq_bindings.free(taken[j]);
        }
        return n;
    }

    struct Starve
    {
        unsigned rounds = 0; // attach-then-retire rounds that took a record
        unsigned free_before = 0;
        unsigned free_after = 0;
        bool line_bindable = true;
        int rc = 0;
        uint32_t cap = 0;
    };
    Starve g_starve;

    // Runs as core zero from inside core one's dispatch entry, so no reclamation can elapse and
    // every record a retirement takes stays taken. One round per line including the line under
    // test, and the carrier's own record is the last: a record is per line, so this leaves the
    // pool exactly empty and one round fewer leaves a record free.
    void starve_the_records_then_claim()
    {
        kickos::irqfix::g_core = 0;
        for (int line = 0; line < KICKOS_MAX_IRQ; line++)
        {
            if (line == LINE_CARRIER)
            {
                continue;
            }
            if (not kickos::irq_attach(line, probe_handler, nullptr))
            {
                continue;
            }
            kickos::irq_detach(line);
            g_starve.rounds++;
        }
        // The precondition, asserted rather than assumed: the line under test cannot be bound.
        // Which refusal this is differs by tree, no record left or the line's own record not
        // yet reclaimed, and the invariant below is the same either way.
        g_starve.line_bindable = kickos::irq_attach(LINE_TARGET, probe_handler, nullptr);
        g_starve.free_before = free_bindings();
        g_starve.rc = kickos::irq_claim(&g_claim_thread, LINE_TARGET, 0u, &g_starve.cap);
        g_starve.free_after = free_bindings();
        kickos::irqfix::g_core = 1;
    }

    TEST_F(IrqQuiesce, ARefusedClaimKeepsNoBindingSlot)
    {
        g_starve = Starve();

        peer_dispatch_running(starve_the_records_then_claim);

        ASSERT_GT(g_starve.rounds, 0u)
            << "no line was bound and retired, so no record was ever taken and this arm drove "
               "nothing";
        ASSERT_FALSE(g_starve.line_bindable)
            << "the line was still bindable, so the claim below was never going to be refused "
               "and this arm asserts nothing about a refusal";
        ASSERT_LT(g_starve.rc, 0) << "the claim was not refused";

        EXPECT_EQ(g_starve.free_after, g_starve.free_before)
            << "the refused claim kept a binding slot. Nothing names it and no publication "
               "record owes it, so it never returns to the pool: repeated refusals exhaust the "
               "binding pool for the life of the image";
        EXPECT_EQ(g_starve.cap, kickos::KCAP_INVALID)
            << "a refused claim handed back a capability";
    }

    // =======================================================================================
    // A line the arch dispatches itself. Its vector reaches a kernel service directly and
    // never kickos_isr_irq, so the line holds no irq_table slot: a capability over it would
    // drive nothing, and the detach that closing the capability performs would mask the line
    // the kernel rings on. The refusal belongs at claim time, a claim that succeeds and cannot
    // be closed safely being worse than one that never happened.

    TEST_F(IrqQuiesce, ClaimOfAnArchOwnedLineIsRefusedAndTouchesNothing)
    {
        kickos::irqfix::g_kernel_owned_line = LINE_FREE;
        unsigned const free_before = free_bindings();
        uint32_t cap = kickos::KCAP_INVALID;

        EXPECT_EQ(kickos::irq_claim(&g_claim_thread, LINE_FREE, 0u, &cap), -KOS_EPERM);
        EXPECT_EQ(cap, kickos::KCAP_INVALID) << "a refused claim handed back a capability";
        EXPECT_EQ(free_bindings(), free_before) << "the refused claim kept a binding slot";
        EXPECT_EQ(kickos::irq_published(LINE_FREE).arg,
                  reinterpret_cast<void*>(static_cast<intptr_t>(LINE_FREE)))
            << "the line names a driver, so the arch vector's service was displaced or a "
               "detach is now owed on a line the kernel rings on";
        EXPECT_EQ(kickos::irqfix::last_line_op(LINE_FREE), -1)
            << "the refused claim reached the controller for a line it does not own";
        EXPECT_FALSE(kickos::irq_attach(LINE_FREE, probe_handler, nullptr))
            << "the other minting entry bound a handler the arch vector never reaches";
    }

    // The control, on the same line and differing only in what the arch declares: the claim
    // takes, the line dispatches to the binding it installed, and closing the capability gives
    // the slot back. Without it the arm above passes on a tree that refuses every claim.
    TEST_F(IrqQuiesce, TheSameLineClaimsAndClosesWhereTheArchReservesNothing)
    {
        kickos::irqfix::g_kernel_owned_line = -1;
        unsigned const free_before = free_bindings();
        uint32_t cap = kickos::KCAP_INVALID;

        ASSERT_TRUE(kickos::irq_attach(LINE_FREE, probe_handler, nullptr));
        kickos::irq_detach(LINE_FREE);
        ASSERT_EQ(kickos::irq_claim(&g_claim_thread, LINE_FREE, 0u, &cap), 0);
        EXPECT_NE(cap, kickos::KCAP_INVALID);
        EXPECT_LT(free_bindings(), free_before) << "the claim took no binding slot";
        EXPECT_NE(kickos::irq_published(LINE_FREE).arg,
                  reinterpret_cast<void*>(static_cast<intptr_t>(LINE_FREE)))
            << "the line still names the null object, so nothing was bound to fire";

        // The detach above already masked the line once, so this is a delta and not a state.
        unsigned const masks_before = kickos::irqfix::count_of(OP_MASK);
        kickos_isr_irq(LINE_FREE);
        EXPECT_EQ(kickos::irqfix::count_of(OP_MASK), masks_before + 1u)
            << "the dispatch did not mask the line it woke a driver for";

        kickos::handle_close(&g_claim_thread, cap);
        EXPECT_EQ(free_bindings(), free_before) << "the closed capability kept its slot";
    }

    // Which core takes a global line is the claimer's, and it is decided once. Decided at
    // unmask instead, it follows whichever core touched the line last, so a driver thread that
    // moved re-targets its own line. The release drops the record, or a re-claim of the line
    // inherits an owner that no longer exists.
    TEST_F(IrqQuiesce, TheClaimRoutesTheLineToItsOwnCoreAndTheReleaseDropsIt)
    {
        uint32_t cap = kickos::KCAP_INVALID;
        kickos::irqfix::g_core = 1;
        pin_claimer(1);

        ASSERT_EQ(kickos::irq_claim(&g_claim_thread, LINE_FREE, 0u, &cap), 0);
        EXPECT_EQ(kickos::irqfix::g_routed_line, LINE_FREE)
            << "the claim routed nothing, so the line is still whatever unmask decides";
        EXPECT_EQ(kickos::irqfix::g_routed_core, 1u)
            << "the line went to a core other than the one that claimed it";

        kickos::irqfix::g_routed_line = -1;
        kickos::handle_close(&g_claim_thread, cap);
        EXPECT_EQ(kickos::irqfix::g_routed_line, LINE_FREE)
            << "the release left the record standing for a re-claim to inherit";
        EXPECT_EQ(kickos::irqfix::g_routed_core, KICKOS_IRQ_ROUTE_NONE)
            << "the release named a core instead of dropping the record";
    }

    // The release masks before it drops the route. The mask goes to the core the route names,
    // so a route dropped first sends it to whichever core an unrouted line falls back to and
    // leaves the claimer's copy of the line armed.
    TEST_F(IrqQuiesce, TheReleaseMasksTheLineBeforeItDropsTheRoute)
    {
        uint32_t cap = kickos::KCAP_INVALID;
        kickos::irqfix::g_core = 1;
        pin_claimer(1);

        ASSERT_EQ(kickos::irq_claim(&g_claim_thread, LINE_FREE, 0u, &cap), 0);
        // What the driver's first wait leaves behind.
        kickos::irq_line_op(LINE_FREE, kickos::LineOp::UNMASK);
        ASSERT_EQ(kickos::irqfix::last_line_op(LINE_FREE),
                  static_cast<int>(kickos::irqfix::OP_UNMASK));

        kickos::irqfix::g_routed_line = -1;
        kickos::handle_close(&g_claim_thread, cap);
        ASSERT_EQ(kickos::irqfix::g_routed_core, KICKOS_IRQ_ROUTE_NONE);
        EXPECT_FALSE(kickos::irqfix::g_routed_armed)
            << "the route was dropped while the line was still armed";
        EXPECT_EQ(kickos::irqfix::last_line_op(LINE_FREE),
                  static_cast<int>(kickos::irqfix::OP_MASK))
            << "the release left the line armed";
    }

    // Test record capacity through line allocation: one record exists per line,
    // so a free line also has a record available. Hold a peer in dispatch to
    // prevent retired records from being reclaimed during the test.

    struct Pool
    {
        unsigned bound = 0;           // lines the pool admitted while records were owed to them
        bool last_took = false;       // the held-back line got the last record
        bool retired_refused = false; // and was refused once that record was retiring
    };
    Pool g_pool;

    void bind_every_line_then_retire_one()
    {
        kickos::irqfix::g_core = 0;
        for (int line = 0; line < KICKOS_MAX_IRQ; line++)
        {
            if (line == LINE_CARRIER or line == LINE_FREE)
            {
                continue;
            }
            if (kickos::irq_attach(line, probe_handler, nullptr))
            {
                g_pool.bound++;
            }
        }
        // Held back so exactly one record is left for it: LINE_CARRIER holds one and the loop
        // above holds the rest.
        g_pool.last_took = kickos::irq_attach(LINE_FREE, probe_handler, nullptr);
        kickos::irq_detach(LINE_FREE);
        g_pool.retired_refused = not kickos::irq_attach(LINE_FREE, probe_handler, nullptr);
        kickos::irqfix::g_core = 1;
    }

    TEST_F(IrqQuiesce, EveryLineBoundTakesEveryRecordAndOneDrainReturnsOne)
    {
        g_pool = Pool();

        peer_dispatch_running(bind_every_line_then_retire_one);

        EXPECT_EQ(g_pool.bound, static_cast<unsigned>(KICKOS_MAX_IRQ - 2))
            << "a line was refused with a record still owed to it: the pool hands out fewer "
               "records than the space holds, so a board cannot bind every line it has";
        EXPECT_TRUE(g_pool.last_took)
            << "the last record was never handed out";
        EXPECT_TRUE(g_pool.retired_refused)
            << "a record was handed out again inside its grace period, with the pool otherwise "
               "empty: the retirement put it back where a dispatch may still hold it";

        // The peer is out, so the next operation on the line drains what that retirement owes
        // and the record it took is the only one there is to hand back.
        EXPECT_TRUE(kickos::irq_attach(LINE_FREE, probe_handler, nullptr))
            << "the retired record never came back, so every record a driver retires is lost "
               "for the lifetime of the image";
    }

    // =======================================================================================
    // A dispatch that already holds the retired pair, against a rebind of its line. The entry
    // snapshots the pair once and the handler masks the line as its first act, so a rebind that
    // arms the line in between leaves the new owner parked on a line the old handler masks.
    //
    // The gate at arch_irq_mask is what makes the window reachable: without it the peer's whole
    // handler runs inside the time core zero needs to retire, rebind and arm, and the
    // interleaving is never taken.

    struct Stale
    {
        std::atomic<int> rebind_rc{-1};
        std::atomic<bool> rebind_armed{false};
        std::atomic<bool> peer_done{false};
        std::atomic<bool> handoff_ok{true};
    };
    Stale g_stale;

    // Core one: enter the dispatch entry of the line being retired and stop in the gate, having
    // read the published pair and taken no lock.
    void peer_stale_dispatch(int line)
    {
        kickos::irqfix::g_core = 1;
        kickos_isr_irq(line);
        g_stale.peer_done.store(true);
    }

    struct IrqStale : public IrqCycle
    {
        // Retire LINE_TARGET and rebind it while a dispatch holding its old pair is stopped
        // between reading that pair and masking the line.
        void rebind_under_a_stale_dispatch()
        {
            kickos::irqfix::hold_next_mask(LINE_TARGET);
            std::thread peer(peer_stale_dispatch, LINE_TARGET);
            if (not wait_for([] { return kickos::irqfix::mask_hold_reached(); }))
            {
                g_stale.handoff_ok.store(false);
            }
            else
            {
                // No lock is held here: the peer needs IrqLock for its post, and each call
                // below takes and drops its own.
                kickos::irqfix::g_core = 0;
                kickos::handle_close(&g_claim_thread, cap);
                uint32_t again = 0;
                int const rc = kickos::irq_claim(&g_claim_thread, LINE_TARGET, 0u, &again);
                g_stale.rebind_rc.store(rc);
                if (rc == 0)
                {
                    // Attach the new binding and rearm its bit, which is what a wait over
                    // that bit does and the only half of the wait this arm is about.
                    bool ok = kickos::irq_bind_notify(&g_claim_thread, again, 0u) == 0;
                    if (ok)
                    {
                        kickos::IrqLock lock;
                        kickos::irq_signallers_rearm(kickos::irqfix::the_notification(), 1u);
                    }
                    g_stale.rebind_armed.store(ok);
                }
            }
            kickos::irqfix::release_mask_hold();
            peer.join();
        }
    };

    // Claim: a line cannot be armed for a new owner while a dispatch holding its retired pair is
    // still in flight. Either the rebind is refused, or it takes and the line is left armed.
    TEST_F(IrqStale, ARebindCannotArmALineUnderAStaleDispatch)
    {
        g_stale.rebind_rc.store(-1);
        g_stale.rebind_armed.store(false);
        g_stale.peer_done.store(false);
        g_stale.handoff_ok.store(true);

        rebind_under_a_stale_dispatch();

        ASSERT_TRUE(g_stale.handoff_ok.load())
            << "the peer never reached the gate, so no dispatch was stopped between reading the "
               "published pair and touching the controller: this arm reproduced nothing";
        ASSERT_FALSE(kickos::irqfix::mask_hold_timed_out())
            << "the gate was never released, so the peer's mask landed on a budget rather than "
               "on this arm's ordering";
        ASSERT_TRUE(g_stale.peer_done.load());
        // The stale dispatch ran its retired pair. The claim below is about ordering the rebind
        // against it, not about stopping it.
        ASSERT_TRUE(target_posted())
            << "the retired handler never posted, so it never reached the controller either";

        EXPECT_TRUE(g_stale.rebind_rc.load() != 0
                    or kickos::irqfix::last_line_op(LINE_TARGET) == kickos::irqfix::OP_UNMASK)
            << "the rebind armed the line and a dispatch holding the RETIRED pair then masked "
               "it. The new owner is parked on a line nothing will raise and no ack of its own "
               "can reopen, while the old semaphore took the post";
    }

    // And the line is not lost either way: with the peer out, a refused rebind lifts, and an
    // admitted one holds the line with the retired binding gone.
    TEST_F(IrqStale, TheRefusedRebindLiftsOnceThePeerLeaves)
    {
        g_stale.rebind_rc.store(-1);
        g_stale.rebind_armed.store(false);
        g_stale.peer_done.store(false);
        g_stale.handoff_ok.store(true);

        rebind_under_a_stale_dispatch();

        ASSERT_TRUE(g_stale.handoff_ok.load()) << "the window was not reproduced";
        ASSERT_TRUE(g_stale.peer_done.load());

        uint32_t again = 0;
        int const rc = kickos::irq_claim(&g_claim_thread, LINE_TARGET, 0u, &again);
        if (g_stale.rebind_rc.load() != 0)
        {
            EXPECT_EQ(rc, 0)
                << "the line was refused after the dispatch holding its retired pair had left, "
                   "so the refusal is a permanent loss of the line rather than a grace period";
        }
        else
        {
            int const held = kickos::irqfix::installed_handle();
            kickos::IrqBinding const* const b = kickos::kernel().irq_bindings.resolve(held);
            ASSERT_NE(b, nullptr) << "the admitted rebind's binding is gone, so the line is lost";
            EXPECT_NE(held, obj) << "the line is still held by the retired binding";
            EXPECT_EQ(b->line, LINE_TARGET);
            EXPECT_EQ(rc, -KOS_EBUSY) << "the line the admitted rebind holds was handed out again";
        }
        EXPECT_EQ(kickos::kernel().irq_bindings.resolve(obj), nullptr)
            << "the retired slot never returned to the pool once the peer had left";
    }
}
