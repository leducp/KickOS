// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The REAL kernel/time/realtime.cc at the posture where the nodes of a partition keep one wall
// clock, one record per node: two nodes, each an instance of this program. A set that is
// stopped inside its writes is a set whose clock read, the one inside realtime_set, hands
// control to the test.

#include <kickos/cap.h>
#include <kickos/instance_local.h>
#include <kickos/irqlock.h>
#include <kickos/time.h>
#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#if not KICKOS_AMP_CLOCK_SHARED or KICKOS_AMP_NODES != 2 or not KICKOS_MULTI_INSTANCE
#error "this gate's posture is one wall clock shared by two nodes, one instance each"
#endif

namespace
{
    std::atomic<uint64_t> g_now{1000u};
    // Run once by the next clock read, as if the reading node stopped there.
    void (*g_stop)() = nullptr;
}

extern "C"
{
    uint64_t arch_clock_now(void)
    {
        void (*const stop)() = g_stop;
        g_stop = nullptr;
        if (stop != nullptr)
        {
            stop();
        }
        return g_now.load();
    }

    arch_irq_state_t arch_irq_save(void)
    {
        return 0;
    }

    void arch_irq_restore(arch_irq_state_t)
    {
    }
}

namespace kickos
{
    namespace detail
    {
        __thread unsigned g_instance_index __attribute__((tls_model("initial-exec"))) = 0;
    }

    bool cap_check_authority(Thread*, uint32_t need)
    {
        return need == AUTH_REALTIME;
    }
}

namespace
{
    // What realtime.cc asked of the timer queue and of the doorbell, per node, by one writer at a
    // time.
    std::atomic<int64_t> g_shifted[2];
    std::atomic<uint64_t> g_armed_at[2];
    std::atomic<uint32_t> g_rings[2];
}

namespace kickos
{
    void ktime_wall_shift(int64_t by_ns, Held)
    {
        std::atomic<int64_t>& s = g_shifted[kickos_instance_index()];
        s.store(s.load() + by_ns);
    }

    void ktime_deadline_arm_at(Thread*, uint64_t deadline_ns, bool wall, Held)
    {
        EXPECT_TRUE(wall) << "a realtime deadline was queued unmarked";
        g_armed_at[kickos_instance_index()] = deadline_ns;
    }

    namespace amp
    {
        void ring(uint32_t to)
        {
            g_rings[to].store(g_rings[to].load() + 1u);
        }
    }
}

namespace
{
    using kickos::instance_select;
    using kickos::realtime_now;
    using kickos::realtime_set;

    constexpr uint64_t A = 0x1111111122222222ull;
    constexpr uint64_t B = 0x3333333344444444ull;
    constexpr uint64_t C = 0x5555555566666666ull;

    void set_on(unsigned node, uint64_t unix_ns)
    {
        instance_select(node);
        ASSERT_EQ(realtime_set(nullptr, unix_ns), 0);
    }

    uint64_t read_on(unsigned node)
    {
        instance_select(node);
        return realtime_now();
    }

    // What node 0 reads while the stop runs, if it returns within a second.
    struct Witness
    {
        std::thread reader;
        std::atomic<bool> done{false};
        uint64_t seen = 0;
    };
    Witness* g_witness = nullptr;

    void read_node0_while_stopped()
    {
        Witness& w = *g_witness;
        w.reader = std::thread([&w] {
            w.seen = read_on(0);
            w.done = true;
        });
        auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (not w.done and std::chrono::steady_clock::now() < until)
        {
            std::this_thread::yield();
        }
    }

    constexpr uint64_t DAY_NS = 86400000000000ull;

    void arm_on(unsigned node, uint64_t unix_ns)
    {
        instance_select(node);
        kickos::IrqLock lock;
        kickos::realtime_deadline_arm(nullptr, unix_ns, lock);
    }

    void rekey_on(unsigned node)
    {
        instance_select(node);
        kickos::IrqLock lock;
        kickos::realtime_rekey(lock);
    }

    class WallShared : public ::testing::Test
    {
      protected:
        void TearDown() override
        {
            g_stop = nullptr;
            instance_select(0);
        }
    };

    TEST_F(WallShared, every_node_reads_the_newest_set_of_any_node)
    {
        set_on(0, A);
        EXPECT_EQ(read_on(1), A);
        set_on(1, B);
        EXPECT_EQ(read_on(0), B);
        EXPECT_EQ(read_on(1), B);
        set_on(0, C);
        EXPECT_EQ(read_on(0), C);
        EXPECT_EQ(read_on(1), C);
        g_now.store(g_now.load() + 5000u);
        EXPECT_EQ(read_on(1), C + 5000u);
    }

    TEST_F(WallShared, a_node_stopped_inside_its_set_leaves_the_last_published_clock_readable)
    {
        set_on(0, A);
        Witness w;
        g_witness = &w;
        g_stop = read_node0_while_stopped;
        set_on(1, B);
        bool const returned = w.done;
        w.reader.join();
        g_witness = nullptr;
        EXPECT_TRUE(returned) << "node 0's read waited on node 1's unfinished set";
        EXPECT_EQ(w.seen, A);
        EXPECT_EQ(read_on(0), B);
    }

    // Two sets that each saw the other's generation unpublished: the higher node's wins.
    TEST_F(WallShared, two_sets_of_one_generation_resolve_to_the_higher_node)
    {
        set_on(0, A);
        g_stop = [] { set_on(0, B); };
        set_on(1, C);
        EXPECT_EQ(read_on(0), C);
        EXPECT_EQ(read_on(1), C);
    }

    // A read never mixes two sets' halves while a peer sets without pause.
    TEST_F(WallShared, a_read_racing_a_peer_that_never_stops_setting_is_never_torn)
    {
        set_on(0, A);
        std::atomic<bool> stop{false};
        std::thread writer([&stop] {
            instance_select(1);
            for (uint32_t k = 1u; not stop; k++)
            {
                uint64_t const v = (static_cast<uint64_t>(k) << 32) | k;
                if (realtime_set(nullptr, v) != 0)
                {
                    break;
                }
            }
        });
        uint32_t torn = 0u;
        for (uint32_t i = 0u; i < 200000u; i++)
        {
            uint64_t const v = read_on(0);
            if (v != A and (v >> 32) != (v & 0xFFFFFFFFu))
            {
                torn++;
            }
        }
        stop = true;
        writer.join();
        EXPECT_EQ(torn, 0u);
    }

    TEST_F(WallShared, a_set_rings_every_other_node_and_not_its_own)
    {
        uint32_t const to0 = g_rings[0];
        uint32_t const to1 = g_rings[1];
        set_on(0, A);
        EXPECT_EQ(g_rings[1], to1 + 1u) << "node 0's set left node 1 unrung";
        EXPECT_EQ(g_rings[0], to0) << "node 0's set rang itself";
        set_on(1, B);
        EXPECT_EQ(g_rings[0], to0 + 1u) << "node 1's set left node 0 unrung";
    }

    // Node 0's deadline is keyed to the clock it read at the park; node 1's set reaches it only
    // through node 0's own re-key, by exactly the set's change, once.
    TEST_F(WallShared, a_peer_set_moves_a_nodes_realtime_deadline_at_its_rekey_alone)
    {
        set_on(0, A);
        arm_on(0, A + 1000000000u);
        EXPECT_EQ(g_armed_at[0], g_now.load() + 1000000000u)
            << "the deadline is not the wall time left, on the monotonic clock";
        int64_t const shifted0 = g_shifted[0];
        set_on(1, A + DAY_NS);
        EXPECT_EQ(g_shifted[0], shifted0) << "a peer's set reached into this node's queue";
        rekey_on(0);
        EXPECT_EQ(g_shifted[0], shifted0 - static_cast<int64_t>(DAY_NS))
            << "the re-key did not move node 0's deadlines by the peer's set";
        rekey_on(0);
        EXPECT_EQ(g_shifted[0], shifted0 - static_cast<int64_t>(DAY_NS))
            << "a second ring with no set moved the deadlines again";
    }

    // A park after a peer's set, before its ring: the park keys itself to the newest clock.
    TEST_F(WallShared, a_park_after_an_unrung_peer_set_keys_to_the_newest_clock)
    {
        set_on(0, A);
        rekey_on(1);
        set_on(0, A + DAY_NS);
        int64_t const shifted1 = g_shifted[1];
        arm_on(1, A + DAY_NS + 1000000u);
        EXPECT_EQ(g_armed_at[1], g_now.load() + 1000000u);
        EXPECT_EQ(g_shifted[1], shifted1 - static_cast<int64_t>(DAY_NS));
    }

    TEST_F(WallShared, a_nodes_own_set_moves_its_own_deadlines)
    {
        set_on(1, A);
        rekey_on(1);
        int64_t const shifted1 = g_shifted[1];
        set_on(1, A - DAY_NS);
        EXPECT_EQ(g_shifted[1], shifted1 + static_cast<int64_t>(DAY_NS))
            << "a set back did not push this node's deadlines out by the day";
    }
}
