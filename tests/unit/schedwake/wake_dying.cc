// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What sched::wake() does with a woken peer while the CURRENT thread is dying, and what
// sched::exit_current() does with the waiters it wakes after its own teardown.
//
// The whole subject is a switch DECISION, which on target has exactly one observable, run
// order, and only under a scheduler that is already running. Here the decision is the
// observable: karch_seam.cc records every arch_switch, so an arm asserts which switch
// happened and WHEN relative to the others, and can seat a state no run reaches.
//
// The two phases of a dying thread are NOT the same and the arms keep them apart:
//   sweep phase   current->dying, state RUNNING, still on the ready structure. A preempting
//                 peer resumes the sweep at the chunk it interrupted.
//   past-exit     state EXITED and off the ready structure, inside exit_current's own waiter
//                 loop. The dying thread will NEVER run again, so a switch here abandons the
//                 rest of that loop.

#include <kickos/cap.h>
#include <kickos/diag.h>
#include <kickos/instance.h>
#include <kickos/sched.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    // The dying thread's own priority in every arm below, chosen mid-range so a peer can sit
    // on either side of it.
    constexpr uint8_t PRIO_DYING = 5;

    // current, RUNNING, at PRIO_DYING, picked by the real scheduler rather than seated by
    // hand: reschedule() is what leaves idle correctly READY behind it. That setup switch is
    // then dropped from the counters, so every arm below counts only its own.
    Thread* running_thread()
    {
        Thread* c = spawn(0, PRIO_DYING);
        sched::reschedule();
        EXPECT_EQ(kernel().current, c) << "fixture: the spawned thread is current";
        g_switches = 0;
        trace_reset();
        return c;
    }

    Thread* blocked_peer(int slot, uint8_t prio)
    {
        Thread* p = spawn(slot, prio);
        kernel().policy->on_remove(p);
        p->state = ThreadState::BLOCKED;
        return p;
    }
}

class SchedWake : public kickos::testfix::KSeam
{
};

// gtest runs a *DeathTest suite ahead of the others, which is the documented placement for a
// forking case.
class SchedWakeDeathTest : public kickos::testfix::KSeam
{
};

// --- the guard, driven directly ----------------------------------------------------------

TEST_F(SchedWake, a_higher_priority_peer_preempts_a_live_thread)
{
    Thread* c = running_thread();
    Thread* p = blocked_peer(1, PRIO_DYING + 1);

    sched::wake(p);

    EXPECT_EQ(g_switches, 1u) << "a live thread yields to a higher-priority wake";
    EXPECT_EQ(kernel().current, p) << "the woken peer is current";
    EXPECT_EQ(c->state, ThreadState::READY) << "the preempted thread is READY";
}

TEST_F(SchedWake, an_equal_priority_peer_does_not_preempt_a_live_thread)
{
    running_thread();
    Thread* p = blocked_peer(1, PRIO_DYING);

    sched::wake(p);

    EXPECT_EQ(g_switches, 0u) << "an equal-priority wake does not preempt";
    EXPECT_EQ(p->state, ThreadState::READY) << "the peer is READY all the same";
}

TEST_F(SchedWake, a_lower_priority_peer_does_not_preempt_a_live_thread)
{
    running_thread();
    Thread* p = blocked_peer(1, PRIO_DYING - 1);

    sched::wake(p);

    EXPECT_EQ(g_switches, 0u) << "a lower-priority wake does not preempt";
}

// The pair that matters: the guard is a priority comparison, not a blanket refusal. The
// equal-priority arm below it is a CONTROL for the same reason the lower-priority one is.
TEST_F(SchedWake, a_dying_thread_defers_an_equal_priority_peer)
{
    Thread* c = running_thread();
    c->dying = true;
    Thread* p = blocked_peer(1, PRIO_DYING);

    sched::wake(p);

    EXPECT_EQ(g_switches, 0u) << "a dying sweep is not interrupted by an equal-priority peer";
    EXPECT_EQ(p->state, ThreadState::READY) << "the deferred peer is still made READY";
    EXPECT_EQ(kernel().current, c) << "the dying thread keeps the CPU";
}

// The arm that separates the guard from its absence. The two above pass with no guard at
// all, because pick_next declines an equal-priority peer while the dying thread is still
// the HEAD of its ready list. It is not always the head: the list is FIFO and an RR slice
// expiry rotates the running thread behind its equals. So the guard is a decision, not an
// optimisation, and this seats the one state where the decision is visible.
TEST_F(SchedWake, a_dying_thread_defers_an_equal_priority_peer_that_pick_next_would_take)
{
    Thread* c = running_thread();
    Thread* ahead = spawn(2, PRIO_DYING); // a second equal-priority thread, left READY
    Thread* p = blocked_peer(1, PRIO_DYING);
    kernel().policy->on_slice_expire(c); // rotates c behind `ahead`
    c->dying = true;

    sched::wake(p);

    EXPECT_EQ(g_switches, 0u) << "the sweep keeps the CPU even when it is not the ready head";
    EXPECT_EQ(kernel().current, c) << "the dying thread keeps the CPU";
    EXPECT_EQ(p->state, ThreadState::READY) << "the deferred peer is still made READY";
    EXPECT_EQ(ahead->state, ThreadState::READY) << "the thread ahead of it did not run";
}

// A CONTROL, not a discriminator: it passes with no guard at all, because pick_next
// declines a lower-priority peer on its own. The arm above is the one that separates the
// guard from its absence, and it covers this case too, since the guard suppresses the
// reschedule whatever pick_next would have returned.
TEST_F(SchedWake, a_dying_thread_defers_a_lower_priority_peer)
{
    Thread* c = running_thread();
    c->dying = true;
    Thread* p = blocked_peer(1, PRIO_DYING - 1);

    sched::wake(p);

    EXPECT_EQ(g_switches, 0u) << "a dying sweep is not interrupted by a lower-priority peer";
    EXPECT_EQ(kernel().current, c) << "the dying thread keeps the CPU";
}

TEST_F(SchedWake, a_dying_thread_yields_to_a_higher_priority_peer)
{
    Thread* c = running_thread();
    c->dying = true;
    Thread* p = blocked_peer(1, PRIO_DYING + 1);

    sched::wake(p);

    EXPECT_EQ(g_switches, 1u) << "a higher-priority peer preempts the sweep";
    EXPECT_EQ(kernel().current, p) << "the woken peer is current";
    // The sweep must be RESUMABLE, which is the whole reason the interleaving is
    // admissible: cap_teardown drops IrqLock between chunks and c is still on the ready
    // structure, so pick_next can return it again.
    EXPECT_EQ(c->state, ThreadState::READY) << "the dying thread stays runnable";
    EXPECT_TRUE(c->dying) << "the dying marker survives the preemption";
}

// --- what a PENDED switch does to the two reads above -------------------------------------

// On ARM, RISC-V and RX arch_switch only PENDS, so the sweep keeps the CPU with `current`
// already naming the peer, to the end of the chunk holding the lock. The stub here returns for
// the same reason, so these arms sit in that state exactly: a second wake in it reads the PEER,
// both guard clauses are dead, and what holds the decision is pick_next rather than the
// clauses. These pin that, in both directions, and the price paid for it.

TEST_F(SchedWake, a_second_wake_under_the_pended_peer_does_not_switch_again)
{
    Thread* c = running_thread();
    Thread* first = blocked_peer(1, PRIO_DYING + 2);
    Thread* under = blocked_peer(2, PRIO_DYING);
    // Rotated off the ready head, as in the guard arms above: without it pick_next declines
    // an equal-priority peer on its own and the second wake proves nothing.
    Thread* ahead = spawn(3, PRIO_DYING);
    kernel().policy->on_slice_expire(c);
    c->dying = true;

    sched::wake(first);
    sched::wake(under);

    EXPECT_STREQ(trace(), "switch1>2") << "the pended switch is not superseded from below it";
    EXPECT_EQ(kernel().current, first) << "the peer published first is still what will run";
    EXPECT_EQ(under->state, ThreadState::READY) << "the second peer is made READY all the same";
    EXPECT_EQ(ahead->state, ThreadState::READY) << "the peer at the sweep's priority did not run";
    EXPECT_EQ(c->state, ThreadState::READY) << "the sweep stays runnable";
    EXPECT_TRUE(c->dying) << "and still marked";
}

TEST_F(SchedWake, a_second_wake_above_the_pended_peer_supersedes_it)
{
    Thread* c = running_thread();
    Thread* first = blocked_peer(1, PRIO_DYING + 1);
    Thread* above = blocked_peer(2, PRIO_DYING + 2);
    c->dying = true;

    sched::wake(first);
    sched::wake(above);

    // The outgoing side of the second switch is the peer, not the sweep whose stack this runs
    // on. Every backend that can be in this state ignores that argument and saves the thread
    // its own switcher finds, so the token below is not a lost context.
    EXPECT_STREQ(trace(), "switch1>2 switch2>3") << "the higher peer supersedes the pended one";
    EXPECT_EQ(kernel().current, above) << "the switch lands on the highest-priority thread";
    EXPECT_EQ(first->state, ThreadState::READY) << "the superseded peer is runnable, not RUNNING";
    EXPECT_EQ(c->state, ThreadState::READY) << "the sweep is still runnable";
}

// The price of that supersede, pinned rather than asserted away: a peer that never ran keeps the
// switch_count and the RR slice its publication armed, so its first quantum starts short.
// Undoing either needs the one fact kernel C cannot have, whether the pended switch has fired,
// so REWRITE this arm rather than delete it on the day the arch reports that.
TEST_F(SchedWake, a_superseded_peer_keeps_the_switch_in_it_never_ran)
{
    Thread* c = running_thread();
    g_now_ns = 1000;
    Thread* first = blocked_peer(1, PRIO_DYING + 1);
    first->policy = Policy::RR;
    first->quantum_ns = 10 * 1000 * 1000;
    Thread* above = blocked_peer(2, PRIO_DYING + 2);
    c->dying = true;

    sched::wake(first);
    sched::wake(above);

    EXPECT_EQ(first->switch_count, 1u) << "charged a switch-in it never ran";
    EXPECT_EQ(first->slice_deadline_ns, g_now_ns + first->quantum_ns)
        << "and holding a slice armed before it ran";
}

// --- the early returns --------------------------------------------------------------------

TEST_F(SchedWake, an_already_ready_peer_is_left_alone_but_loses_its_deadline)
{
    Thread* c = running_thread();
    Thread* p = spawn(1, PRIO_DYING + 1);
    p->on_timer = true;

    // READY, not BLOCKED: the funnel drops the deadline BEFORE it tests the state, so a
    // wake that races the timer cannot leave a stale deadline behind.
    sched::wake(p);

    EXPECT_FALSE(p->on_timer) << "the deadline is dropped before the state test";
    EXPECT_EQ(g_switches, 0u) << "an already-ready peer is not re-readied and does not switch";
    EXPECT_EQ(kernel().current, c) << "current is unchanged";
    EXPECT_EQ(c->state, ThreadState::RUNNING) << "a wake does not demote the current thread";
}

TEST_F(SchedWake, a_wake_before_the_first_pick_does_not_switch)
{
    Thread* p = blocked_peer(1, PRIO_DYING);
    // sched::init leaves current null and sched::start seats it. No reachable pre-start
    // waker was found in the tree, so this pins the ASYMMETRY rather than a live path:
    // tick_rr guards the same pointer and this funnel must too.
    kernel().current = nullptr;

    sched::wake(p);

    EXPECT_EQ(p->state, ThreadState::READY) << "the peer becomes READY with no current thread";
    EXPECT_EQ(g_switches, 0u) << "a pre-start wake does not switch";
}

// EXITED is the ThreadPool's free marker (thread.h): the slot is reclaimable BECAUSE the state
// says so, and nothing else records it. So readying an exited thread does not merely resurrect
// it, it takes the slot out of the pool with no way back. No caller reaches this today, since an
// exiting thread is on no queue and carries WAIT_NONE, and the funnel is where it is closed
// because `state == EXITED` is load-bearing two lines further down.
TEST_F(SchedWake, an_exited_thread_is_not_woken_and_its_slot_stays_free)
{
    Thread* c = running_thread();
    Thread* dead = seat_pool(0, PRIO_DYING + 1);
    kernel().policy->on_remove(dead);
    dead->state = ThreadState::EXITED;

    sched::wake(dead);

    EXPECT_EQ(dead->state, ThreadState::EXITED) << "an exited thread is not made READY";
    EXPECT_EQ(kernel().ready_bitmap & (1u << dead->prio), 0u) << "and no ready list holds it";
    EXPECT_EQ(g_switches, 0u) << "nothing switched to it";
    EXPECT_EQ(kernel().current, c) << "current is unchanged";
    EXPECT_EQ(kernel().threads.alloc(), 0) << "the pool still reads the slot as free";
}

// The guard compares the EFFECTIVE priority, and nothing else would do: a dying thread
// routinely carries a priority-inheritance boost above its own anchor, and comparing the
// anchor would admit every peer sitting between the two. Seats exactly that gap.
TEST_F(SchedWake, the_guard_compares_the_effective_priority_not_the_anchor)
{
    Thread* c = running_thread();
    c->base_prio = PRIO_DYING - 2; // boosted: prio 5, anchor 3
    // Rotated off the ready head for the same reason as the arm above: without it a
    // reschedule declines on its own and the comparison is unobservable.
    Thread* ahead = spawn(2, PRIO_DYING);
    Thread* p = blocked_peer(1, PRIO_DYING - 1); // between the anchor and the boost
    kernel().policy->on_slice_expire(c);
    c->dying = true;

    sched::wake(p);

    EXPECT_EQ(g_switches, 0u) << "a peer under the boost is deferred, not compared to the anchor";
    EXPECT_EQ(kernel().current, c) << "the boosted dying thread keeps the CPU";
    EXPECT_EQ(ahead->state, ThreadState::READY) << "the thread ahead of it did not run";
}

// --- the exit path, run for real ----------------------------------------------------------

// The sharpest arm in the gate. exit_current wakes its join waiters AFTER its own
// on_remove, when it can never be scheduled again, and its final reschedule is meant to
// be the ONE switch. A guard narrowed on priority alone lets the first higher-priority
// joiner switch away mid-loop: on every hardware port the switch is deferred to the
// IrqLock release and the loop survives, on the sim swapcontext takes the CPU there and
// then, and the remaining waiters are never woken and kickos_terminate never runs. Both
// orders leave every waiter READY, so only the trace separates them.
TEST_F(SchedWake, the_exit_sweep_wakes_every_joiner_before_its_single_switch)
{
    Thread* c = running_thread();
    Thread* w_lower = seat_pool(0, PRIO_DYING + 1);
    Thread* w_higher = seat_pool(1, PRIO_DYING + 4);
    // A joiner of a DIFFERENT thread, in the same pool scan. The sweep keys on the wait
    // edge, not on being parked, so a scan that matched any waiter would forge its
    // completion.
    Thread* other = seat_pool(2, PRIO_DYING + 2);
    park_join(w_lower, c);
    park_join(w_higher, c);
    park_join(other, w_lower);

    run_exit(0);

    EXPECT_EQ(g_switches, 1u) << "exit_current requests exactly one switch";
    // The reclaim token dates the switch: it runs immediately after the sweep and before
    // the waiter loop, so a switch AFTER it is a switch that waited for the whole loop.
    EXPECT_STREQ(trace(), "reclaim switch1>11")
        << "the single switch is the last thing exit_current does";
    // The wake evidence is the cleared edge, not the run state: the final reschedule
    // picks the higher joiner, so one of the two is RUNNING rather than READY and a
    // state-only assertion would read as a missed wake.
    EXPECT_EQ(w_lower->wait_join_target(), nullptr) << "the first joiner's edge was cleared";
    EXPECT_EQ(w_higher->wait_join_target(), nullptr) << "the second joiner's edge was cleared";
    EXPECT_EQ(w_lower->state, ThreadState::READY) << "the joiner not picked is left READY";
    EXPECT_EQ(w_higher->state, ThreadState::RUNNING) << "the highest joiner is the pick";
    EXPECT_EQ(w_lower->wait_result, 0) << "the first joiner was told the join completed";
    EXPECT_EQ(w_higher->wait_result, 0) << "and so was the second";
    EXPECT_EQ(other->state, ThreadState::BLOCKED) << "a joiner of another thread stays parked";
    EXPECT_NE(other->wait_join_target(), nullptr) << "and keeps its wait edge";
    EXPECT_EQ(c->state, ThreadState::EXITED) << "the dying thread is EXITED";
    EXPECT_EQ(g_parked, 1u) << "the exited thread parked once";
}

// THE arm the repair exists for, and the only one that drives the real cap_teardown. Every
// arm above calls sched::wake directly; this one puts a live CAP_WAIT entry through
// obj_close_protocol's endpoint branch, so the wake comes from inside the sweep, from the
// one site whose woken peer is NOT priority-bounded: a PLAIN sender boosts nothing, where
// the mutex force-unlock and the reply EPIPE both leave the dying thread boosted at or
// above the peer they wake.
//
// The ORDER is the whole assertion, and it needs the reclaim token to state it: the switch
// count is 1 either way, and its from/to pair is the same pair either way, because after a
// mid-sweep switch the final reschedule finds the sender already current and declines.
// Only "switch before reclaim" says the sweep was interrupted rather than completed.
TEST_F(SchedWake, a_plain_sender_epiped_by_the_sweep_preempts_it_mid_sweep)
{
    Thread* c = running_thread();
    attach_caps(c, KICKOS_CAP_FIRST_DYNAMIC + 1);
    Thread* sender = spawn(1, PRIO_DYING + 3);
    spawn(2, PRIO_DYING - 1); // keeps k.live above zero past the sweep

    Endpoint* ep = endpoint();
    int const ep_handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
    endpoint_server_set(ep, c);
    // The WAIT right, not the type, is what makes this the last receiver: the sweep's
    // endpoint arm fires the EPIPE drain when recv_holders reaches 0, and only a
    // WAIT-bearing cap counts toward it.
    ep->recv_holders = 1;
    cap_install_at(c, KICKOS_CAP_FIRST_DYNAMIC, ep_handle, CapType::CAP_ENDPOINT, CAP_WAIT);
    park_plain_sender(sender, ep);
    g_switches = 0;
    trace_reset();

    run_exit(0);

    EXPECT_STREQ(trace(), "switch1>2 reclaim") << "the sweep is preempted BEFORE it finishes";
    EXPECT_EQ(g_switches, 1u) << "the preemption is the only switch";
    EXPECT_EQ(sender->state, ThreadState::RUNNING) << "the EPIPEd sender got the CPU";
    EXPECT_EQ(sender->wait_result, -KOS_EPIPE) << "and it was woken with EPIPE";
    EXPECT_EQ(c->state, ThreadState::EXITED) << "the dying thread still finished its exit";
    EXPECT_EQ(ep->server, nullptr) << "the endpoint's server pointer was cleared";
    EXPECT_FALSE(cap_teardown_active()) << "the sweep still balanced its depth";
}

// The site design section 8.2 first called "bounded by construction" and is NOT. The bound
// is a snapshot taken when the waiter parked: mutex_lock raised the owner then, and
// sched::set_prio on a BLOCKED thread propagates nothing, so a later boost of the waiter
// never reaches the owner. mutex_force_unlock then wakes a peer that outranks the dying
// thread, and the real sweep is what gets preempted.
TEST_F(SchedWake, a_mutex_waiter_boosted_past_the_dying_owner_preempts_the_sweep)
{
    Thread* c = running_thread();
    attach_caps(c, KICKOS_CAP_FIRST_DYNAMIC + 1);
    Thread* waiter = spawn(1, PRIO_DYING + 4); // boosted past the owner after parking
    spawn(2, PRIO_DYING - 1);                  // keeps k.live above zero

    int mtx_handle = 0;
    Mutex* m = own_mutex(c, &mtx_handle);
    cap_install_at(c, KICKOS_CAP_FIRST_DYNAMIC, mtx_handle, CapType::CAP_MUTEX, CAP_WAIT);
    park_mutex_waiter(waiter, m);
    g_switches = 0;
    trace_reset();

    run_exit(0);

    EXPECT_STREQ(trace(), "switch1>2 reclaim")
        << "the force-unlock preempts the sweep it runs inside";
    EXPECT_EQ(m->owner, waiter) << "ownership transferred to the woken waiter";
    EXPECT_EQ(waiter->wait_result, -KOS_EOWNERDEAD) << "and it was told the owner died";
    EXPECT_EQ(c->held_list, nullptr) << "the dying thread released the mutex it held";
}

// Every other sweep arm fits in ONE teardown chunk, which leaves the guard's own safety
// argument ungated: the sweep drops IrqLock between chunks and RESUMES. This one puts a
// live cap on each side of a chunk boundary and wakes from the FIRST, so there is real
// work left after the preemption. cap_teardown's own totality asserts fire if it does not
// finish, so reaching the end at all is half the oracle.
TEST_F(SchedWake, a_preempted_sweep_resumes_and_finishes_the_next_chunk)
{
    Thread* c = running_thread();
    uint32_t const width = KICKOS_CAP_FIRST_DYNAMIC + KCAP_TEARDOWN_CHUNK + 1;
    attach_caps(c, width);
    Thread* sender = spawn(1, PRIO_DYING + 3);
    Thread* waiter = spawn(2, PRIO_DYING + 1);
    spawn(3, PRIO_DYING - 1); // keeps k.live above zero

    // The endpoint cap in the FIRST chunk: its EPIPE drain is what preempts.
    Endpoint* ep = endpoint();
    int const ep_handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
    endpoint_server_set(ep, c);
    ep->recv_holders = 1;
    cap_install_at(c, KICKOS_CAP_FIRST_DYNAMIC, ep_handle, CapType::CAP_ENDPOINT, CAP_WAIT);
    park_plain_sender(sender, ep);

    // The mutex cap in the LAST slot, past the boundary: only a resumed sweep reaches it.
    int mtx_handle = 0;
    Mutex* m = own_mutex(c, &mtx_handle);
    cap_install_at(c, static_cast<int>(width) - 1, mtx_handle, CapType::CAP_MUTEX, CAP_WAIT);
    park_mutex_waiter(waiter, m);
    g_switches = 0;
    trace_reset();

    run_exit(0);

    EXPECT_EQ(m->owner, waiter) << "the cap past the chunk boundary was released too";
    EXPECT_EQ(c->held_list, nullptr) << "the sweep completed its held list";
    EXPECT_EQ(ep->server, nullptr) << "and the cap before the boundary was released";
    EXPECT_EQ(sender->wait_result, -KOS_EPIPE) << "the first chunk's sender was EPIPEd";
    EXPECT_EQ(waiter->wait_result, -KOS_EOWNERDEAD) << "the last chunk's waiter was woken";
    EXPECT_FALSE(cap_teardown_active()) << "the sweep balanced its depth across the boundary";
}

// Two negative controls for the drain, because the positive arm alone leaves the
// conditions it fires on unpinned: with one holder and one WAIT-bearing cap, deleting the
// WAIT test or the recv_holders test changes nothing observable.
TEST_F(SchedWake, a_sender_is_not_epiped_while_another_receiver_holds_the_endpoint)
{
    Thread* c = running_thread();
    attach_caps(c, KICKOS_CAP_FIRST_DYNAMIC + 1);
    Thread* sender = spawn(1, PRIO_DYING + 3);
    spawn(2, PRIO_DYING - 1);

    Endpoint* ep = endpoint();
    int const ep_handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
    endpoint_server_set(ep, c);
    ep->recv_holders = 2; // a live peer still holds a WAIT cap on this endpoint
    kernel().endpoint_refs[kernel().endpoints.index_of(ep)] = 2;
    cap_install_at(c, KICKOS_CAP_FIRST_DYNAMIC, ep_handle, CapType::CAP_ENDPOINT, CAP_WAIT);
    park_plain_sender(sender, ep);
    g_switches = 0;
    trace_reset();

    run_exit(0);

    EXPECT_STREQ(trace(), "reclaim switch1>3") << "no drain, so the only switch is the exit's own";
    EXPECT_EQ(sender->state, ThreadState::BLOCKED) << "the sender is still parked";
    EXPECT_EQ(ep->recv_holders, 1u) << "one holder was dropped, not the last";
}

TEST_F(SchedWake, a_send_only_cap_does_not_drain_the_endpoint)
{
    Thread* c = running_thread();
    attach_caps(c, KICKOS_CAP_FIRST_DYNAMIC + 1);
    Thread* sender = spawn(1, PRIO_DYING + 3);
    spawn(2, PRIO_DYING - 1);

    Endpoint* ep = endpoint();
    int const ep_handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
    ep->recv_holders = 1;
    // CAP_SIGNAL, not CAP_WAIT: closing a send-only cap is not a receiver leaving, so it
    // must not touch recv_holders and must not EPIPE anybody.
    cap_install_at(c, KICKOS_CAP_FIRST_DYNAMIC, ep_handle, CapType::CAP_ENDPOINT, CAP_SIGNAL);
    park_plain_sender(sender, ep);
    g_switches = 0;
    trace_reset();

    run_exit(0);

    EXPECT_EQ(sender->state, ThreadState::BLOCKED) << "a send-only close strands nobody awake";
    EXPECT_EQ(ep->recv_holders, 1u) << "recv_holders is untouched by a send-only close";
}

TEST_F(SchedWake, the_drain_epipes_every_parked_sender_not_just_the_first)
{
    Thread* c = running_thread();
    attach_caps(c, KICKOS_CAP_FIRST_DYNAMIC + 1);
    Thread* hi = spawn(1, PRIO_DYING + 3);
    Thread* lo = spawn(2, PRIO_DYING + 1);
    spawn(3, PRIO_DYING - 1);

    Endpoint* ep = endpoint();
    int const ep_handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
    endpoint_server_set(ep, c);
    ep->recv_holders = 1;
    cap_install_at(c, KICKOS_CAP_FIRST_DYNAMIC, ep_handle, CapType::CAP_ENDPOINT, CAP_WAIT);
    park_plain_sender(hi, ep);
    park_plain_sender(lo, ep);
    g_switches = 0;
    trace_reset();

    run_exit(0);

    EXPECT_EQ(hi->wait_result, -KOS_EPIPE) << "the highest sender was EPIPEd";
    EXPECT_EQ(lo->wait_result, -KOS_EPIPE) << "and so was the one behind it";
    EXPECT_TRUE(ep->send_waiters.empty()) << "the drain emptied the queue";
}

TEST_F(SchedWake, the_exit_sweep_reclaims_the_console_once)
{
    running_thread();
    // Keeps k.live above zero. Without it exit_current reaches kickos_terminate, which
    // the fixture refuses by name rather than ending the process with a status.
    seat_pool(0, PRIO_DYING + 1);

    run_exit(0);

    EXPECT_EQ(g_console_reclaimed, 1u) << "the console reclaim runs once after the sweep";
    EXPECT_EQ(g_console_noted, 0u) << "no endpoint arm noted a console death";
    EXPECT_FALSE(cap_teardown_active()) << "the sweep left no depth behind";
}

// --- the one invariant enforced by a panic ------------------------------------------------

// detach_current is the whole blocking funnel's entry, and from ISR context arch_switch
// defers while the supposedly blocked thread keeps running, so the kernel refuses with a
// kpanic instead of a return code. g_in_isr exists for exactly this.
TEST_F(SchedWakeDeathTest, blocking_from_isr_context_panics)
{
    running_thread();
    g_in_isr = true;

    KICKOS_EXPECT_PANIC(sched::detach_current(), diag::kBlockInIsr);
}
