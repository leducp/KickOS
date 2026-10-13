// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An own-image AMP node's writer a peer's claim refuses, end to end over the REAL
// kernel/init/console.cc, console_tx.cc and park.cc and the REAL virt_arm64 claim: it parks until
// that claim's release rings its node, or until the grant's deadline. A ring is taken at once
// where this node is unmasked and once the writer has parked where it is masked. Two writers of
// one node run on contexts of their own, so one can stay parked while the other runs.

#include <stdint.h>
#include <ucontext.h>

#include <string>

#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "arch/arm64/chip/virt_arm64/console_claim.h"
#include "dark_seam.h"
#include "isolated_arm.h"
#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    namespace claim = kickos::virt_arm64::console;

    constexpr uint8_t PRIO_WRITER = 8;
    constexpr uint8_t PRIO_SECOND = 9;
    constexpr uint32_t BUDGET = 6400u; // ticks, read as microseconds
    constexpr uint32_t NODES = 2u;
    constexpr uint32_t SELF_NODE = 1u;
    constexpr uint32_t PEER_NODE = 0u;

    class ConsoleClaim : public KSeam
    {
    };

    uint64_t g_word = 0;
    uint32_t g_now = 0;
    uint32_t g_waiting[NODES] = {};
    bool g_rung = false;

    claim::Claim g_self{SELF_NODE + 1u, NODES};
    claim::Claim g_peer{PEER_NODE + 1u, NODES};

    // The peer's release, made from inside this node's claim wait right after its offer.
    bool g_release_after_check = false;
    // The peer's release, frees the word, and this node's claim wait runs before the release reads
    // the marks.
    bool g_split_release = false;
    bool g_inside_release = false;
    uint32_t g_last_wait_us = 0;
    bool g_deadline_hit = false;
    uint8_t g_park_kind = WAIT_NONE;

    // The doorbell service this node runs for a ring.
    void deliver(void)
    {
        if (not g_rung)
        {
            return;
        }
        g_rung = false;
        IrqLock lock;
        console_claim_rung(lock);
    }

    // The writer's deadline, where nothing woke it first: past it, the peer's grant has ended.
    void expire_if_parked(Thread* parked)
    {
        if (parked->state != ThreadState::BLOCKED)
        {
            return;
        }
        g_deadline_hit = true;
        g_now = g_now + BUDGET + 1u;
        IrqLock lock;
        thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
    }

    // Two writers of this node, each on a context of its own; a park returns to the arm.
    ucontext_t g_arm_ctx;
    ucontext_t g_writer_ctx[2];
    ucontext_t g_parked_ctx[2];
    alignas(16) char g_writer_stack[2][256u * 1024u];
    Thread* g_writers[2] = {};
    bool g_returned[2] = {};
    int g_parks = 0;

    int writer_of(Thread* t)
    {
        if (t == g_writers[0])
        {
            return 0;
        }
        return 1;
    }

    void park_to_arm(Thread* parked)
    {
        wake_next_park(park_to_arm);
        g_parks = g_parks + 1;
        swapcontext(&g_parked_ctx[writer_of(parked)], &g_arm_ctx);
    }

    void start(int w, void (*body)())
    {
        getcontext(&g_writer_ctx[w]);
        g_writer_ctx[w].uc_stack.ss_sp = g_writer_stack[w];
        g_writer_ctx[w].uc_stack.ss_size = sizeof(g_writer_stack[w]);
        g_writer_ctx[w].uc_link = &g_arm_ctx;
        makecontext(&g_writer_ctx[w], body, 0);
        swapcontext(&g_arm_ctx, &g_writer_ctx[w]);
    }

    void stage(void)
    {
        darkseam::reset();
        g_word = 0;
        g_now = 1000u;
        g_waiting[0] = 0;
        g_waiting[1] = 0;
        g_rung = false;
        g_release_after_check = false;
        g_split_release = false;
        g_inside_release = false;
        g_last_wait_us = 0;
        g_deadline_hit = false;
        g_park_kind = WAIT_NONE;
        Thread* const writer = seat_pool(0, PRIO_WRITER);
        attach_caps(writer, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        EXPECT_EQ(sched::current(), writer);
    }

    // The peer is mid-line, 1000 ticks into its grant.
    void peer_holds(void)
    {
        ASSERT_TRUE(g_peer.try_acquire(BUDGET));
        g_now = g_now + 1000u;
    }

    std::string const kLine = "node one\n";
}

namespace kickos::virt_arm64::console
{
    uint32_t ticks(void)
    {
        return g_now;
    }

    uint64_t load(void)
    {
        return g_word;
    }

    bool compare_exchange(uint64_t& expected, uint64_t desired)
    {
        if (g_word != expected)
        {
            expected = g_word;
            return false;
        }
        g_word = desired;
        if (g_inside_release and desired == 0)
        {
            g_inside_release = false;
            g_last_wait_us = g_self.wait(BUDGET);
        }
        return true;
    }

    bool wait_room(void)
    {
        return true;
    }

    void store(char c)
    {
        darkseam::g_wire.push_back(c);
    }

    // Bounded: a claim's owner is only a constructor argument, and a zero one names node -1.
    uint32_t waiting(uint32_t node)
    {
        if (node >= NODES)
        {
            ADD_FAILURE() << "the claim read the waiting word of node " << node;
            return 0;
        }
        return g_waiting[node];
    }

    void set_waiting(uint32_t node, uint32_t value)
    {
        if (node >= NODES)
        {
            ADD_FAILURE() << "the claim marked node " << node << " waiting";
            return;
        }
        g_waiting[node] = value;
    }

    void fence(void)
    {
    }

    void ring(uint32_t node)
    {
        if (node != SELF_NODE)
        {
            return;
        }
        g_rung = true;
        if (irq_depth() == 0)
        {
            deliver();
        }
    }
}

extern "C"
{
    int arch_console_write_retry(char const* buf, size_t n, bool* cr_pending)
    {
        bool held = false;
        {
            IrqLock lock;
            held = g_self.open(BUDGET);
        }
        if (not held)
        {
            return 0;
        }
        IrqLock lock;
        return static_cast<int>(g_self.user_write(buf, n, cr_pending, BUDGET));
    }

    uint32_t arch_console_claim_wait(void)
    {
        if (g_split_release)
        {
            g_split_release = false;
            g_inside_release = true;
            g_peer.release();
            return g_last_wait_us;
        }
        g_last_wait_us = g_self.wait(BUDGET);
        if (g_last_wait_us != 0 and g_release_after_check)
        {
            g_release_after_check = false;
            g_peer.release();
        }
        return g_last_wait_us;
    }
}

// The writer parks for what is left of the peer's grant, and the peer's release rings this node
// before that: the line goes out after the peer's, and the deadline never comes.
TEST_F(ConsoleClaim, a_writer_the_claim_refuses_is_woken_by_the_release)
{
    run_isolated([]() {
        stage();
        ASSERT_NO_FATAL_FAILURE(peer_holds());
        wake_next_park([](Thread* parked) {
            g_park_kind = parked->wait_kind;
            EXPECT_EQ(darkseam::g_wire, "") << "a byte went out under the peer's grant";
            g_peer.release();
            deliver();
            expire_if_parked(parked);
        });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), true),
                  static_cast<int>(kLine.size()));
        EXPECT_EQ(g_park_kind, WAIT_CONSOLE) << "the writer did not park on the console";
        EXPECT_EQ(g_last_wait_us, BUDGET - 1000u) << "the park is not the grant's remainder";
        EXPECT_FALSE(g_deadline_hit) << "the release did not wake the writer: it waited out its "
                                        "deadline";
        EXPECT_EQ(darkseam::g_wire, kLine);
    });
}

// The release lands after the writer's offer and before its park, while this node is masked: the
// ring is taken once the writer has parked, and still wakes it.
TEST_F(ConsoleClaim, a_release_between_the_offer_and_the_park_still_wakes_the_writer)
{
    run_isolated([]() {
        stage();
        ASSERT_NO_FATAL_FAILURE(peer_holds());
        g_release_after_check = true;
        wake_next_park([](Thread* parked) {
            g_park_kind = parked->wait_kind;
            deliver();
            expire_if_parked(parked);
        });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), true),
                  static_cast<int>(kLine.size()));
        EXPECT_EQ(g_park_kind, WAIT_CONSOLE) << "the writer did not park on the console";
        EXPECT_FALSE(g_deadline_hit) << "the release's ring was lost: the writer waited out its "
                                        "deadline";
        EXPECT_EQ(darkseam::g_wire, kLine);
    });
}

// A holder that never releases rings nobody: the writer waits out the grant and takes it.
TEST_F(ConsoleClaim, a_writer_whose_holder_never_releases_waits_out_the_deadline)
{
    run_isolated([]() {
        stage();
        ASSERT_NO_FATAL_FAILURE(peer_holds());
        wake_next_park([](Thread* parked) {
            deliver();
            expire_if_parked(parked);
        });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), true),
                  static_cast<int>(kLine.size()));
        EXPECT_TRUE(g_deadline_hit);
        EXPECT_EQ(darkseam::g_wire, kLine);
    });
}

// One writer parked on the peer's grant; the second offers while the peer's release is under way,
// after it freed the word and before it reads the marks. The second takes the claim and clears the
// node's mark, so that release rings nobody: the take itself must wake the first.
TEST_F(ConsoleClaim, a_take_inside_a_release_still_wakes_the_nodes_other_writer)
{
    run_isolated([]() {
        stage();
        ASSERT_NO_FATAL_FAILURE(peer_holds());
        g_writers[0] = sched::current();
        g_writers[1] = seat_pool(1, PRIO_SECOND);
        attach_caps(g_writers[1], KICKOS_CAP_CHILD_WIDTH);
        g_parks = 0;
        wake_next_park(park_to_arm);
        start(0, []() { (void)kconsole_write_user(kLine.data(), kLine.size(), true); });
        ASSERT_EQ(g_parks, 1) << "fixture: the first writer did not park";
        ASSERT_EQ(sched::current(), g_writers[1]);
        start(1, []() {
            g_split_release = true;
            g_returned[1] = kconsole_write_user("two\n", 4, true) == 4;
        });
        EXPECT_TRUE(g_returned[1]);
        EXPECT_EQ(darkseam::g_wire, "two\n");
        EXPECT_EQ(g_waiting[SELF_NODE], 0u) << "fixture: the take did not clear the mark";
        EXPECT_NE(g_writers[0]->state, ThreadState::BLOCKED)
            << "the first writer stayed parked: the take cleared the mark, so the release rang "
               "nobody";
        EXPECT_EQ(g_writers[0]->wait_result, 0);
    });
}

// The peer never releases and takes a fresh grant each time one ends: each parked writer returns
// once its own deadline passes, after one park, to offer its line again.
TEST_F(ConsoleClaim, both_writers_return_by_the_deadline_of_a_peer_that_never_releases)
{
    run_isolated([]() {
        stage();
        ASSERT_NO_FATAL_FAILURE(peer_holds());
        g_writers[0] = sched::current();
        g_writers[1] = seat_pool(1, PRIO_SECOND);
        g_parks = 0;
        wake_next_park(park_to_arm);
        start(0, []() { g_returned[0] = console_claim_wait() == 0; });
        ASSERT_EQ(sched::current(), g_writers[1]);
        start(1, []() { g_returned[1] = console_claim_wait() == 0; });
        ASSERT_EQ(g_parks, 2) << "fixture: both writers did not park";
        g_now = g_now + BUDGET + 1u;
        ASSERT_TRUE(g_peer.try_acquire(BUDGET));
        {
            IrqLock lock;
            thread_abort_park(g_writers[0], -KOS_ETIMEDOUT, lock);
            thread_abort_park(g_writers[1], -KOS_ETIMEDOUT, lock);
        }
        swapcontext(&g_arm_ctx, &g_parked_ctx[0]);
        swapcontext(&g_arm_ctx, &g_parked_ctx[1]);
        EXPECT_TRUE(g_returned[0]) << "the first writer did not return at its deadline";
        EXPECT_TRUE(g_returned[1]) << "the second writer did not return at its deadline";
        EXPECT_EQ(g_parks, 2) << "a writer parked again past its deadline";
    });
}
