// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The end-to-end benchmark's publication order, read off the state each fake below sees when
// the shipped bench.cc calls it.

#include <gtest/gtest.h>

#include <type_traits>

#include "bench.cc"

namespace
{
    enum Call
    {
        CURRENT,
        CLOCK,
        INJECT,
        UNLOCK
    };

    struct Seen
    {
        Call call;
        uint32_t mode;
        uint64_t t0;
    };

    constexpr uint32_t SEEN_MAX = 8;
    Seen g_seen[SEEN_MAX];
    uint32_t g_seen_n = 0;
    uint64_t g_clock = 0;
    // What the clock writes into the state as it is read, so an arm can tell a read of the state
    // before the stamp from one after it.
    uint32_t g_state_at_clock = 0xFFFFFFFFu;
    kickos::Thread g_waiter{};
    kickos::Thread* g_current = &g_waiter;

    constexpr int LINE = 3;

    void note(Call call)
    {
        if (g_seen_n < SEEN_MAX)
        {
            g_seen[g_seen_n] = Seen{call, g_e2e_mode, g_e2e_t0};
        }
        g_seen_n++;
    }

    // <call>'s first record, or a record no arm expects.
    Seen seen(Call call)
    {
        for (uint32_t i = 0; i < g_seen_n and i < SEEN_MAX; i++)
        {
            if (g_seen[i].call == call)
            {
                return g_seen[i];
            }
        }
        return Seen{call, 0xFFFFFFFFu, 0};
    }

    uint32_t count(Call call)
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < g_seen_n and i < SEEN_MAX; i++)
        {
            if (g_seen[i].call == call)
            {
                n++;
            }
        }
        return n;
    }

    class E2ePublish : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            g_e2e_mode = E2E_IDLE;
            g_e2e_waiter = nullptr;
            g_e2e_t0 = 0;
            kickos::g_bench_e2e_isr_core = kickos::BENCH_CORE_NONE;
            g_current = &g_waiter;
            g_clock = 1000;
            g_state_at_clock = 0xFFFFFFFFu;
            g_seen_n = 0;
        }
    };
}

namespace kickos
{
    namespace detail
    {
        constinit InstanceLocal<Kernel> g_instance;
    }

    Thread* sched::current()
    {
        note(CURRENT);
        return g_current;
    }

    void sched::set_affinity(Thread*, uint32_t)
    {
    }

    void klock_enter()
    {
    }

    void klock_leave()
    {
        note(UNLOCK);
    }

    bool irq_attach(int, IrqHandler, void*)
    {
        return true;
    }

    void irq_line_op(int, LineOp, Held)
    {
    }

    void kprintf_paced(char const*, ...)
    {
    }
}

extern "C"
{
    uint64_t arch_clock_now(void)
    {
        note(CLOCK);
        if (g_state_at_clock != 0xFFFFFFFFu)
        {
            g_e2e_mode = g_state_at_clock;
        }
        g_clock += 100;
        return g_clock;
    }

    bool arch_irq_inject(int)
    {
        note(INJECT);
        return true;
    }

    arch_irq_state_t arch_irq_save(void)
    {
        return 0;
    }

    void arch_irq_restore(arch_irq_state_t)
    {
    }

    uint32_t arch_cpu_id(void)
    {
        return 0;
    }

    uint64_t arch_cpu_clock_hz(void)
    {
        return 1000000;
    }

    void arch_ipi_send(uint32_t)
    {
    }

    void arch_ipi_wait(uint32_t)
    {
    }

    bool arch_ipi_answered(uint32_t)
    {
        return true;
    }
}

// The order no host run can show: on x86 an acquire, a release and a relaxed access are the
// same MOV, so the type is what holds it.
TEST_F(E2ePublish, the_state_acquires_and_releases)
{
    EXPECT_TRUE((std::is_same<decltype(g_e2e_mode),
                              kickos::Atomic<uint32_t, kickos::Order::ACQUIRE
                                                           | kickos::Order::RELEASE>>::value));
}

TEST_F(E2ePublish, the_arm_publishes_after_naming_its_waiter_and_before_the_unlock)
{
    ASSERT_EQ(kickos::bench_e2e_arm(LINE), 0);
    EXPECT_EQ(seen(CURRENT).mode, E2E_IDLE) << "ARMED published before the waiter was named";
    EXPECT_EQ(seen(UNLOCK).mode, E2E_ARMED) << "ARMED published after the lock dropped";
    EXPECT_EQ(g_e2e_waiter, &g_waiter);
}

TEST_F(E2ePublish, the_park_mark_reads_the_state_before_it_names_the_waiter)
{
    kickos::bench_e2e_park_mark();
    EXPECT_EQ(count(CURRENT), 0u) << "the waiter was named before the state was read";
    EXPECT_EQ(g_e2e_mode, E2E_IDLE);

    g_e2e_mode = E2E_ARMED;
    g_e2e_waiter = &g_waiter;
    kickos::bench_e2e_park_mark();
    EXPECT_EQ(seen(CURRENT).mode, E2E_ARMED) << "PARKED published before the waiter was named";
    EXPECT_EQ(g_e2e_mode, E2E_PARKED);
}

TEST_F(E2ePublish, the_raise_reads_the_park_then_publishes_its_stamp_before_the_injection)
{
    g_e2e_waiter = &g_waiter;
    g_e2e_mode = E2E_ARMED;
    EXPECT_EQ(kickos::bench_e2e_raise(), -KOS_EAGAIN);
    EXPECT_EQ(count(CLOCK), 0u) << "the stamp was taken before the park was read";

    g_e2e_mode = E2E_PARKED;
    kickos::g_bench_e2e_line = LINE;
    ASSERT_EQ(kickos::bench_e2e_raise(), 0);
    EXPECT_EQ(seen(CLOCK).mode, E2E_PARKED) << "RAISED published before the stamp";
    Seen const inject = seen(INJECT);
    EXPECT_EQ(inject.mode, E2E_RAISED) << "RAISED published after the injection";
    EXPECT_EQ(inject.t0, g_clock) << "the stamp written after the injection";
}

TEST_F(E2ePublish, the_close_reads_the_state_between_its_stamp_and_the_identity_test)
{
    g_e2e_waiter = &g_waiter;
    g_e2e_epoch = g_waiter.switch_count;
    g_waiter.switch_count = g_waiter.switch_count + 1;
    kickos::g_bench_e2e_isr_core = 0;
    g_e2e_t0 = g_clock;
    g_e2e_mode = E2E_RAISED;
    ASSERT_EQ(kickos::bench_e2e_close(), 0);
    EXPECT_EQ(seen(CURRENT).mode, E2E_IDLE) << "the state was read after the identity test";

    g_e2e_mode = E2E_RAISED;
    g_state_at_clock = E2E_IDLE;
    EXPECT_EQ(kickos::bench_e2e_close(), -KOS_EINVAL)
        << "the state was read before the closing stamp";
}
