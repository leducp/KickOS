// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A slay whose victim is RUNNING on a PEER core, at two kernel cores, over the real
// thread_cancel_kind, the real available_to and the real switch_book.
//
// The claim on a victim is a switch INTO it: switch_book rebuilds its context into
// kickos_thread_slay_exit before arch_switch. So a core that keeps re-picking its own running
// victim never claims it, and the slayer's core reaches nothing that runs on a peer. Three
// things have to happen, in order, and each is one line of kernel:
//
//   the ask          thread_cancel_kind publishes the reschedule against the victim's core.
//   the displacement available_to refuses a slain, unclaimed current, so that core's pass
//                    takes the victim OFF the core and owes itself one more pass.
//   the claim        the pass it owed picks the victim, now merely READY, and redirects it.
//
// THE SELF-OWED PASS IS A CELL AND NOT A SEND: klock_resched_ask strips the caller's own bit
// deliberately, so switch_book publishes the cell alone and a RELEASE of the kernel lock is
// what fires the raise. WHICH release is per backend, so this gate reads the raise as present
// or absent and never as a count: a swap that swaps inline releases twice over one booking
// (kickos_switch_unlock inside the swap, then klock_leave), and one BOOKED from an interrupt
// releases once, klock_detach having left `owed` set across the klock_leave that follows it.
// Both halves are read here, the cell through kickos_kernel_core_resched_owed and the raise
// through the seam's arch_ipi_resched_self.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/klock.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class SlayPeer : public KSeam
            {
            };

            constexpr uint32_t CORE_ME = 0;   // the core the fixture speaks as
            constexpr uint32_t CORE_PEER = 1; // the core the victim executes on

            // NOT ROOT_INDEX: ThreadPool::alloc retires root's slot, and is_root reads the
            // slot comparison, so an arm seating its threads there speaks about root.
            constexpr int SLOT_SLAYER = 1;
            constexpr int SLOT_VICTIM = 2;
            constexpr int SLOT_HOG = 3;
            // Fixture storage rather than a pool slot: nothing scans for an idle thread.
            constexpr int SLOT_PEER_IDLE = 0;

            constexpr uint8_t PRIO_SLAYER = 6;
            constexpr uint8_t PRIO_VICTIM = 5;
            constexpr uint8_t PRIO_HOG = 7;
            constexpr uint8_t PRIO_HOG_STEPPED = 3;
            static_assert(PRIO_HOG > PRIO_VICTIM and PRIO_HOG_STEPPED < PRIO_VICTIM,
                          "the load has to outrank the victim before it steps aside and to "
                          "rank below it after, or the arms below assert about whichever "
                          "thread the prio scan reached first");

            // Distinct from any real address the fixture holds, so an assertion on the stack
            // TOP cannot be satisfied by a coincidence.
            void* const STACK_BASE = reinterpret_cast<void*>(static_cast<uintptr_t>(0x20040000u));
            void* const STACK_BASE_SLAYER =
                reinterpret_cast<void*>(static_cast<uintptr_t>(0x20050000u));
            constexpr size_t STACK_SIZE = 0x1000u;

            // The cell is keyed by TARGET and read from the target's seat, so an arm asking
            // about a peer has to speak as that peer for the length of the read.
            int owed_at(uint32_t core)
            {
                uint32_t const was = g_core;
                g_core = core;
                int const owed = kickos_kernel_core_resched_owed();
                g_core = was;
                return owed;
            }

            // klock.cc's sequence rows outlive reset(), so an arm starts from what it drains.
            void drain(uint32_t core)
            {
                uint32_t const was = g_core;
                g_core = core;
                (void)kickos_kernel_core_resched_take();
                g_core = was;
            }

            // Every counter this gate reads, back to zero, with the rows drained. An arm that
            // seats anything after place() starts again from here: sched::add pokes every
            // peer running below the new thread, so a seat of its own leaves a cell standing.
            void settle()
            {
                drain(CORE_ME);
                drain(CORE_PEER);
                g_ipi_sends = 0;
                g_ipi_send_mask = 0;
                g_ipi_self_raises = 0;
                g_redirects = 0;
                g_redirect_target = nullptr;
                g_redirect_entry = nullptr;
                g_redirect_stack_top = 0;
                trace_reset();
            }

            struct Placed
            {
                Thread* slayer; // RUNNING on CORE_ME
                Thread* victim; // RUNNING on CORE_PEER, with a stack of its own
                Thread* peer_idle;
            };

            // CORE_PEER needs an idle of its own: pick_next's fallback is idle[core]
            // unconditionally, and the fixture seats one for the boot core alone.
            Thread* seat_peer_idle()
            {
                Thread* const t = &g_fx.t[SLOT_PEER_IDLE];
                t->base_prio = KICKOS_PRIO_IDLE;
                t->prio = KICKOS_PRIO_IDLE;
                t->id = static_cast<uint16_t>(SLOT_PEER_IDLE + 1);
                sched::add_idle(t, CORE_PEER);
                return t;
            }

            // The victim RUNNING on the peer, seated by hand for the reason no core may pull a
            // thread off another's CPU: only that core's own pass moves it.
            Placed place()
            {
                Placed p{};
                p.slayer = seat_pool(SLOT_SLAYER, PRIO_SLAYER);
                p.victim = seat_pool(SLOT_VICTIM, PRIO_VICTIM);
                p.victim->stack_base = STACK_BASE;
                p.victim->stack_size = STACK_SIZE;
                p.peer_idle = seat_peer_idle();
                {
                    IrqLock lock;
                    sched::reschedule();
                }

                p.victim->state = ThreadState::RUNNING;
                kernel().current[CORE_PEER] = p.victim;

                settle();
                return p;
            }

            // As the peer core, for one scheduler pass. The seat travels with the identity:
            // every keyed read in the compiled sources resolves through g_core.
            void pass_as_peer()
            {
                uint32_t const was = g_core;
                g_core = CORE_PEER;
                {
                    IrqLock lock;
                    sched::reschedule();
                }
                g_core = was;
            }

            // A slay ASKED BY THE PEER, so the identity has to hold for the whole lock span:
            // klock keeps a per-core row, and an acquire as one core with the release as
            // another leaves the depth of a core that never took it.
            void slay_as_peer(Thread* t)
            {
                uint32_t const was = g_core;
                g_core = CORE_PEER;
                {
                    IrqLock lock;
                    thread_cancel_kind(t, CANCEL_SLAY);
                }
                g_core = was;
            }

            // seat_pool seats every core's bit, so without this a core that has displaced its
            // own victim takes the PEER's off the ready lists on its next pass, and an arm
            // about two cores resolving two victims would read one core doing both.
            void pin_each_to_its_core(Placed const& p)
            {
                p.slayer->affinity = 1u << CORE_ME;
                p.victim->affinity = 1u << CORE_PEER;
            }

            // A thread that outranks the victim, READY on both cores' ready lists.
            Thread* seat_hog()
            {
                Thread* const hog = seat_pool(SLOT_HOG, PRIO_HOG);
                settle();
                return hog;
            }
        }

        TEST_F(SlayPeer, a_slay_of_a_peers_running_thread_asks_that_peer_and_nobody_else)
        {
            Placed p = place();
            ASSERT_EQ(kernel().current[CORE_ME], p.slayer) << "fixture: the slayer runs here";
            ASSERT_EQ(kernel().current[CORE_PEER], p.victim)
                << "fixture: the victim runs on the peer core";
            ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: nothing stands against the peer core";

            {
                IrqLock lock;
                thread_cancel_kind(p.victim, CANCEL_SLAY);
            }

            EXPECT_EQ(p.victim->cancel_kind, CANCEL_SLAY) << "fixture: the escalation stood";
            EXPECT_EQ(g_ipi_send_mask, 1u << CORE_PEER)
                << "the victim spins at the unprivileged level on a core nobody told to "
                   "reschedule, so it is slain only when something unrelated displaces it, and "
                   "kos_thread_slay's caller waits on that";
            EXPECT_NE(owed_at(CORE_PEER), 0)
                << "the cell is what outlives the raise: an acquire loop's poll may absorb the "
                   "doorbell edge without entering a scheduler, and only the cell carries the "
                   "ask to the next release or dispatch";
            EXPECT_EQ(g_ipi_self_raises, 0u)
                << "the slayer's own core took a raise for a victim it cannot reach";
        }

        // The displacement, which is what a core owes its own running victim. Reddens on a tree
        // whose available_to answers sched_placeable_on here: pick_next returns the victim,
        // reschedule finds next == current, and the victim keeps the core.
        TEST_F(SlayPeer, the_victims_own_pass_takes_the_core_off_it_and_owes_itself_another)
        {
            Placed p = place();
            {
                IrqLock lock;
                thread_cancel_kind(p.victim, CANCEL_SLAY);
            }
            drain(CORE_PEER);

            pass_as_peer();

            EXPECT_EQ(kernel().current[CORE_PEER], p.peer_idle)
                << "the core re-picked its own slain thread: the claim is a switch INTO the "
                   "victim, so a core that never switches away never claims it";
            EXPECT_EQ(p.victim->state, ThreadState::READY)
                << "fixture: the switch stored the state a claim reads";
            EXPECT_EQ(g_redirects, 0u)
                << "the claim fired on the pass that displaced the victim, where the outgoing "
                   "thread's live registers are still saved over prev->ctx";
            EXPECT_NE(owed_at(CORE_PEER), 0)
                << "the victim is READY and unclaimed and its core owes itself nothing: the "
                   "claim then waits on a tick, a syscall or another thread, none of which is "
                   "bounded";
            EXPECT_GT(g_ipi_self_raises, 0u)
                << "the cell was published with no raise behind it, so the pass it names is "
                   "taken only when something else enters the scheduler";
        }

        TEST_F(SlayPeer, the_pass_the_core_owed_itself_claims_the_victim)
        {
            Placed p = place();
            {
                IrqLock lock;
                thread_cancel_kind(p.victim, CANCEL_SLAY);
            }
            pass_as_peer();
            ASSERT_EQ(p.victim->state, ThreadState::READY) << "fixture: the victim lost the core";

            pass_as_peer();

            EXPECT_EQ(kernel().current[CORE_PEER], p.victim)
                << "the second pass did not pick the victim, so nothing runs its teardown";
            EXPECT_EQ(g_redirect_target, p.victim) << "the rebuild named another thread";
            EXPECT_EQ(reinterpret_cast<void*>(g_redirect_entry),
                      reinterpret_cast<void*>(&kickos_thread_slay_exit))
                << "the victim runs its OWN teardown through the same exit_current every other "
                   "death uses";
            EXPECT_EQ(g_redirect_stack_top,
                      reinterpret_cast<uintptr_t>(STACK_BASE) + STACK_SIZE)
                << "at the TOP of its own stack, not at the depth it had reached";
        }

        // A kill keeps its cleanup window: it reaches its own death point on its own schedule,
        // so nothing here may displace it. The two verbs must stay distinct at the point they
        // diverge, which is the claim slayhook makes for the redirect half.
        TEST_F(SlayPeer, a_kill_of_a_peers_running_thread_asks_nobody_and_displaces_nothing)
        {
            Placed p = place();

            {
                IrqLock lock;
                thread_cancel_kind(p.victim, CANCEL_KILL);
            }

            ASSERT_EQ(p.victim->cancel_kind, CANCEL_KILL) << "fixture: the escalation stood";
            EXPECT_EQ(g_ipi_sends, 0u) << "a kill poked a peer that has nothing to do about it";
            EXPECT_EQ(owed_at(CORE_PEER), 0) << "a kill published a reschedule against a peer";

            pass_as_peer();

            EXPECT_EQ(kernel().current[CORE_PEER], p.victim)
                << "a cooperative cancel took the core off its target, which is what a slay is "
                   "for";
            EXPECT_EQ(g_redirects, 0u) << "a kill's target had its context rebuilt";
        }

        // cap_teardown releases IrqLock between chunks, so a victim can be preempted with the
        // claim already fired. Displacing it there would hand the core to somebody else with
        // the sweep half done, and re-claiming it would restart that sweep from the top.
        TEST_F(SlayPeer, a_victim_already_inside_its_own_teardown_is_neither_displaced_nor_reclaimed)
        {
            Placed p = place();
            p.victim->cancel_kind = CANCEL_SLAY;
            p.victim->dying = true;

            pass_as_peer();

            EXPECT_EQ(kernel().current[CORE_PEER], p.victim)
                << "a thread partway through its own capability sweep lost the core";
            EXPECT_EQ(owed_at(CORE_PEER), 0)
                << "a pass was owed for a claim that has already fired";
            EXPECT_EQ(g_ipi_self_raises, 0u)
                << "a raise was owed for a claim that has already fired";
            EXPECT_EQ(g_redirects, 0u)
                << "the sweep was restarted from the top over a table that is already partly "
                   "empty";
        }

        // A core cannot ask itself through klock_resched_ask, which strips the caller's bit, so
        // a slayer whose victim is its own current must not reach it at all: that thread is
        // inside the kernel and reads the escalation on its way out.
        TEST_F(SlayPeer, a_slayer_whose_victim_is_its_own_current_asks_nobody)
        {
            Placed p = place();
            ASSERT_EQ(sched::current(), p.slayer) << "fixture: the slayer is this core's current";

            {
                IrqLock lock;
                thread_cancel_kind(p.slayer, CANCEL_SLAY);
            }

            EXPECT_EQ(g_ipi_sends, 0u) << "a core sent itself a doorbell it strips anyway";
            EXPECT_EQ(owed_at(CORE_ME), 0)
                << "a core published a cell against itself from the cancel path, where the ask "
                   "belongs to the switch that makes the thread takeable";
            EXPECT_EQ(g_ipi_self_raises, 0u) << "a raise was owed with no cell behind it";
        }

        // The refusal has to be NARROW: an ordinary RUNNING thread keeps the core it is on,
        // which is the whole of migration (a re-mask is what moves one, not a bare pick).
        TEST_F(SlayPeer, an_unslain_running_thread_keeps_its_core_across_its_own_pass)
        {
            Placed p = place();

            pass_as_peer();

            EXPECT_EQ(kernel().current[CORE_PEER], p.victim)
                << "a core dropped a thread it was legitimately running, so every pass on every "
                   "core now costs a switch";
            EXPECT_EQ(g_ipi_self_raises, 0u) << "an ordinary pass owed itself another";
        }

        // TWO SLAYS CROSSING, each core's current the other core's victim. The cell is keyed
        // by the PAIR that asks and answers, so the two asks stand independently and the
        // dispatch that answers one leaves the other where it is.
        TEST_F(SlayPeer, mutual_slays_ask_each_others_core_and_neither_cell_answers_the_other)
        {
            Placed p = place();

            {
                IrqLock lock;
                thread_cancel_kind(p.victim, CANCEL_SLAY);
            }
            slay_as_peer(p.slayer);

            ASSERT_EQ(p.victim->cancel_kind, CANCEL_SLAY) << "fixture: the escalation stood";
            ASSERT_EQ(p.slayer->cancel_kind, CANCEL_SLAY) << "fixture: the escalation stood";
            EXPECT_EQ(g_ipi_send_mask, (1u << CORE_ME) | (1u << CORE_PEER))
                << "one of the two slays reached no core: each victim is RUNNING somewhere, "
                   "and only its own core's pass can take it off";
            EXPECT_NE(owed_at(CORE_PEER), 0) << "the slayer's ask published no cell";
            EXPECT_NE(owed_at(CORE_ME), 0) << "the peer's ask published no cell";
            EXPECT_GT(g_ipi_self_raises, 0u)
                << "a core released the kernel lock with an ask standing against it and let "
                   "the doorbell edge go: the cell outlives the raise, so every release has "
                   "to carry it again";

            drain(CORE_PEER);

            ASSERT_EQ(owed_at(CORE_PEER), 0) << "fixture: the peer's dispatch took its ask";
            EXPECT_NE(owed_at(CORE_ME), 0)
                << "the peer taking the ask against it took the one against the slayer's core "
                   "too, so one of two crossing slays is answered by a pass on the wrong core "
                   "and its victim is never claimed";
        }

        // And each is claimed by the core its victim ran on, on that core's own stack: the
        // claim is a switch INTO the victim, so nothing about it crosses cores.
        TEST_F(SlayPeer, mutual_slays_are_each_claimed_by_the_core_their_victim_ran_on)
        {
            Placed p = place();
            pin_each_to_its_core(p);
            p.slayer->stack_base = STACK_BASE_SLAYER;
            p.slayer->stack_size = STACK_SIZE;

            {
                IrqLock lock;
                thread_cancel_kind(p.victim, CANCEL_SLAY);
            }
            slay_as_peer(p.slayer);
            drain(CORE_ME);
            drain(CORE_PEER);

            pass_as_peer();
            {
                IrqLock lock;
                sched::reschedule();
            }

            EXPECT_EQ(kernel().current[CORE_PEER], p.peer_idle)
                << "the peer re-picked its own slain current";
            EXPECT_NE(kernel().current[CORE_ME], p.slayer)
                << "the slayer's core re-picked its own slain current: a slay reaches its "
                   "victim through a switch, and the core that asked is no exception";
            EXPECT_EQ(g_redirects, 0u)
                << "a claim fired on a pass that displaced a victim, where the outgoing "
                   "thread's live registers are still saved over prev->ctx";
            EXPECT_NE(owed_at(CORE_PEER), 0) << "the peer owes itself no pass for its victim";
            EXPECT_NE(owed_at(CORE_ME), 0)
                << "the slayer's core owes itself no pass for the victim it displaced, so a "
                   "core that slays its own current waits on an unrelated event";

            drain(CORE_ME);
            drain(CORE_PEER);

            pass_as_peer();

            EXPECT_EQ(kernel().current[CORE_PEER], p.victim)
                << "the pass the peer owed itself did not claim the victim";
            EXPECT_EQ(g_redirect_target, p.victim) << "the rebuild named another thread";
            EXPECT_EQ(g_redirect_stack_top, reinterpret_cast<uintptr_t>(STACK_BASE) + STACK_SIZE)
                << "the peer's victim was redirected onto a stack that is not its own";

            {
                IrqLock lock;
                sched::reschedule();
            }

            EXPECT_EQ(kernel().current[CORE_ME], p.slayer)
                << "the pass the slayer's core owed itself did not claim its own victim";
            EXPECT_EQ(g_redirect_target, p.slayer) << "the rebuild named another thread";
            EXPECT_EQ(reinterpret_cast<void*>(g_redirect_entry),
                      reinterpret_cast<void*>(&kickos_thread_slay_exit))
                << "the second victim runs something other than its own teardown";
            EXPECT_EQ(g_redirect_stack_top,
                      reinterpret_cast<uintptr_t>(STACK_BASE_SLAYER) + STACK_SIZE)
                << "the two claims agreed on one stack, so one victim tears down over the "
                   "other's frames";
            EXPECT_EQ(g_redirects, 2u) << "one of the two claims fired twice";
        }

        // A victim no core is running needs no displacement and gets no ask: it is claimed by
        // the first pass that would have picked it anyway, which is the pass its PRIORITY
        // earns it and not one the slay brings forward.
        TEST_F(SlayPeer, a_slain_ready_victim_waits_behind_higher_priority_and_asks_no_core)
        {
            Placed p = place();
            Thread* const hog = seat_hog();
            pass_as_peer();
            ASSERT_EQ(kernel().current[CORE_PEER], hog)
                << "fixture: the higher priority thread holds the peer's core";
            ASSERT_EQ(p.victim->state, ThreadState::READY)
                << "fixture: the victim is on the ready lists and on no core";
            settle();

            {
                IrqLock lock;
                thread_cancel_kind(p.victim, CANCEL_SLAY);
            }

            EXPECT_EQ(g_ipi_sends, 0u)
                << "a slay poked a core that is not running its victim: the ask exists to "
                   "take a core OFF the victim, and this one holds somebody else";
            EXPECT_EQ(owed_at(CORE_PEER), 0) << "a cell stands against a core that owes the "
                                                "victim nothing but an ordinary pick";
            EXPECT_EQ(owed_at(CORE_ME), 0) << "a cell stands against the slayer's own core";

            pass_as_peer();

            EXPECT_EQ(kernel().current[CORE_PEER], hog)
                << "the claim displaced a higher priority thread: it is an ordinary switch "
                   "in, and a slay that preempts hands a dying thread the CPU ahead of a live "
                   "one";
            EXPECT_EQ(g_redirects, 0u) << "the context was rebuilt on a pass that never "
                                          "picked the victim";
            EXPECT_EQ(p.victim->state, ThreadState::READY) << "fixture: the victim still waits";

            {
                IrqLock lock;
                sched::set_prio(hog, PRIO_HOG_STEPPED);
            }
            pass_as_peer();

            EXPECT_EQ(kernel().current[CORE_PEER], p.victim)
                << "the first pass that could pick the victim did not, so a slain READY "
                   "thread is never claimed at all";
            EXPECT_EQ(g_redirect_target, p.victim) << "the rebuild named another thread";
            EXPECT_EQ(reinterpret_cast<void*>(g_redirect_entry),
                      reinterpret_cast<void*>(&kickos_thread_slay_exit))
                << "the victim runs something other than its own teardown";
            EXPECT_EQ(g_redirect_stack_top, reinterpret_cast<uintptr_t>(STACK_BASE) + STACK_SIZE)
                << "at the TOP of its own stack, not at the depth it had reached";
        }

        // The slay lands while the victim's core is already switching it out FOR A REASON OF
        // ITS OWN, so the pass that ask names is spent on somebody else. What keeps the claim
        // alive is the switch itself owing one: the booking is about the victim losing the
        // core unclaimed, never about where the core went instead.
        TEST_F(SlayPeer, a_slay_landing_as_the_victims_core_switches_it_out_owes_a_pass_anyway)
        {
            Placed p = place();
            Thread* const hog = seat_hog();
            ASSERT_EQ(kernel().current[CORE_PEER], p.victim)
                << "fixture: the victim still holds the peer's core";

            {
                IrqLock lock;
                thread_cancel_kind(p.victim, CANCEL_SLAY);
            }
            drain(CORE_PEER);
            ASSERT_EQ(g_ipi_self_raises, 0u) << "fixture: no core has raised its own doorbell";

            pass_as_peer();

            EXPECT_EQ(kernel().current[CORE_PEER], hog)
                << "fixture: the pass had a reason of its own to switch, which is the higher "
                   "priority thread";
            EXPECT_EQ(p.victim->state, ThreadState::READY)
                << "fixture: the switch stored the state a claim reads";
            EXPECT_EQ(g_redirects, 0u)
                << "the claim fired on a pass that switched to somebody else";
            EXPECT_NE(owed_at(CORE_PEER), 0)
                << "the pass a switched-out victim owes its core is booked for some switches "
                   "and not others, so a victim displaced by an unrelated preemption is owed "
                   "nothing and the ask that displaced it has already been spent";
            EXPECT_GT(g_ipi_self_raises, 0u)
                << "the cell was published with no raise behind it, so the pass it names is "
                   "taken only when something else enters the scheduler";
        }
    }
}
