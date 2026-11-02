// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kickos::driver::bring_up as the driver task's entry thread runs it, over the recording seam in
// kos_seam.h: what it refuses before it spawns, the threads it spawns into its own task with
// their roles taken from the entry thread's layout, the readiness barrier its later threads
// wait on, the roles thread 0 keeps, and the trap that ends the task once a thread exists.
//
// An arm checking an order compares the FULL ordered trace as one string; a counter or a
// substring test drops it.

#include <kickos/sys/driver_service.h>

#include <kickos/sys/errno.h>

#include "kos_seam.h"

#include <gtest/gtest.h>

#include <string>

#include <signal.h>
#include <stdint.h>
#include <string.h>

namespace drv = kickos::driver;

namespace
{
    bool says(char const* msg, char const* needle)
    {
        return strstr(msg, needle) != nullptr;
    }

    static_assert(KOS_SPAWN_DELEGATED_CAP0 == 1 and KOS_CAP_WAIT == 1 and KOS_CAP_SIGNAL == 2,
                  "the expected traces name delegated indices and rights by value");

    constexpr uintptr_t K_BASE = 0x4000c000u;
    constexpr uint32_t K_BLOCK = 256u;
    constexpr uint16_t K_READY_OFF = 8u;
    constexpr uint8_t K_PRIO = 8u;

    void t_body0(void*)
    {
        kos_seam_note("body0");
    }

    void t_body1(void*)
    {
        kos_seam_note("body1");
    }

    void t_body2(void*)
    {
        kos_seam_note("body2");
    }

    int g_block_init_rc = 0;

    int block_init(void* blk, struct kos_driver_instance const*)
    {
        drv::ready_latch(blk, K_READY_OFF)->store(0u);
        return g_block_init_rc;
    }

    drv::Launch g_launch[drv::KOS_DRV_THREADS_MAX];

    // The buffered UART's shape: an IRQ thread holding the window, the notification and the line,
    // and a service thread past the barrier receiving and ringing its doorbell.
    constexpr drv::Descriptor k_uart = {
        .tag = "[drvuart] ",
        .expected_base = K_BASE,
        .block_size = K_BLOCK,
        .block_flags = 0,
        .ready_offset = K_READY_OFF,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .line_count = 1,
        .thread_count = 2,
        .barrier_after = 1,
        .lines = {{KOS_IRQ_LEVEL}},
        .threads = {{.entry = t_body0,
                     .name = "drvirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0}}},
                    {.entry = t_body1,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_NOTIFY, KOS_CAP_SIGNAL, drv::doorbell_badge(1)}}}},
        .block_init = block_init,
        .launch = g_launch
    };
    static_assert(drv::valid(k_uart) and drv::ring_doorbell_shape_ok(k_uart, K_READY_OFF, K_BLOCK),
                  "the UART-shaped gate descriptor is not a driver shape");

    // The line witness's shape: two lines on thread 0, a receiver, and a raiser holding line 0 for
    // SIGNAL alone, with no barrier.
    constexpr drv::Descriptor k_three = {
        .tag = "[drvthree] ",
        .expected_base = 0,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .line_count = 2,
        .thread_count = 3,
        .barrier_after = 3,
        .lines = {{KOS_IRQ_EDGE}, {KOS_IRQ_EDGE}},
        .threads = {{.entry = t_body0,
                     .name = "irq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_LINE0_INDEX,
                     .window_grant = false,
                     .cap_count = 3,
                     .caps = {{drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT, 0}}},
                    {.entry = t_body1,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}},
                    {.entry = t_body2,
                     .name = "raiser",
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_SIGNAL, 0}}}},
        .block_init = nullptr,
        .launch = nullptr
    };
    static_assert(drv::valid(k_three), "the three-thread gate descriptor is not a driver shape");

    // One thread receiving, the sim console's shape.
    constexpr drv::Descriptor k_one = {
        .tag = "[drvone] ",
        .expected_base = 0,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_body0,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr,
        .launch = nullptr
    };
    static_assert(drv::valid(k_one), "the one-thread gate descriptor is not a driver shape");

    // The bus shape: one thread holding the endpoint, then the notification and the line.
    constexpr drv::Descriptor k_bus = {
        .tag = "[drvbus] ",
        .expected_base = K_BASE,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .line_count = 1,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {{KOS_IRQ_EDGE}},
        .threads = {{.entry = t_body0,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_LINE0_INDEX,
                     .window_grant = true,
                     .cap_count = 3,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr,
        .launch = nullptr
    };
    static_assert(drv::valid(k_bus), "the bus gate descriptor is not a driver shape");

    // Thread 0 holding nothing, its receiver a worker: the driver witness's trapping driver.
    constexpr drv::Descriptor k_idle = {
        .tag = "[drvidle] ",
        .expected_base = 0,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .line_count = 0,
        .thread_count = 2,
        .barrier_after = 2,
        .lines = {},
        .threads = {{.entry = t_body0,
                     .name = "entry",
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 0,
                     .caps = {}},
                    {.entry = t_body1,
                     .name = "worker",
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr,
        .launch = nullptr
    };
    static_assert(drv::valid(k_idle), "the idle-entry gate descriptor is not a driver shape");

    // The threads that wait on a role are essential; a raiser and an entry holding nothing are not.
    static_assert(drv::essential(k_three.threads[0]) and drv::essential(k_three.threads[1])
                      and not drv::essential(k_three.threads[2]),
                  "an IRQ and a service thread are essential, a raiser is not");
    static_assert(not drv::essential(k_idle.threads[0]) and drv::essential(k_idle.threads[1]),
                  "an entry holding nothing is not essential, its receiver is");

    // The layout each states to the init: thread 0's roles first, then the rest in token order.
    constexpr drv::Layout L_UART = drv::layout_of(k_uart);
    static_assert(L_UART.count == 4 and L_UART.at[drv::KOS_DRV_TOK_NOTIFY] == 0
                      and L_UART.at[drv::KOS_DRV_TOK_LINE0] == 1 and L_UART.at[drv::KOS_DRV_TOK_EP] == 2
                      and L_UART.at[drv::KOS_DRV_TOK_DOORBELL] == 3,
                  "the UART shape's entry layout is notify, line 0, ep, doorbell");
    constexpr drv::Layout L_THREE = drv::layout_of(k_three);
    static_assert(L_THREE.count == 4 and L_THREE.at[drv::KOS_DRV_TOK_LINE0 + 1] == 2
                      and L_THREE.at[drv::KOS_DRV_TOK_EP] == 3
                      and L_THREE.at[drv::KOS_DRV_TOK_DOORBELL] == drv::KOS_DRV_TOK_NONE,
                  "the line shape's entry layout is notify, line 0, line 1, ep");
    constexpr drv::Layout L_BUS = drv::layout_of(k_bus);
    static_assert(L_BUS.count == 3 and L_BUS.at[drv::KOS_DRV_TOK_EP] == 0
                      and L_BUS.at[drv::KOS_DRV_TOK_NOTIFY] == 1 and L_BUS.at[drv::KOS_DRV_TOK_LINE0] == 2,
                  "the bus shape's entry layout is its own order");
    constexpr drv::Layout L_IDLE = drv::layout_of(k_idle);
    static_assert(L_IDLE.count == 1 and L_IDLE.at[drv::KOS_DRV_TOK_EP] == 0,
                  "an entry holding nothing still holds the endpoint it hands on");

    // L3: a window is the entry thread's, which the kernel seats it on.
    constexpr drv::Descriptor window_on(drv::Descriptor d, uint8_t i)
    {
        d.threads[0].window_grant = false;
        d.threads[i].window_grant = true;
        return d;
    }
    static_assert(not drv::valid(window_on(k_uart, 1)), "L3 must refuse a window on a thread past 0");

    // L8: a latch nobody waits on, or nobody sets, and one whose waiters have nowhere to start.
    constexpr drv::Descriptor barrier_at(drv::Descriptor d, uint8_t after)
    {
        d.barrier_after = after;
        return d;
    }
    static_assert(not drv::valid(barrier_at(k_uart, 2)), "L8 must refuse a barrier past the last thread");
    static_assert(not drv::valid(barrier_at(k_uart, 0)), "L8 must refuse a barrier before thread 0");
    constexpr drv::Descriptor unlaunched(drv::Descriptor d)
    {
        d.launch = nullptr;
        return d;
    }
    static_assert(not drv::valid(unlaunched(k_uart)), "L8 must refuse a barrier with no launch records");

    // L14: thread 0 naming a token twice, and a doorbell the init does not mint.
    constexpr drv::Descriptor twice(drv::Descriptor d)
    {
        d.threads[0].caps[1] = {drv::KOS_DRV_RES_EP, KOS_CAP_SIGNAL, 0};
        return d;
    }
    static_assert(not drv::valid(twice(k_bus)), "L14 must refuse thread 0 naming a token twice");
    constexpr drv::Descriptor odd_bell(drv::Descriptor d)
    {
        d.threads[1].caps[1].badge = drv::doorbell_badge(2);
        return d;
    }
    static_assert(not drv::valid(odd_bell(k_uart)), "L14 must refuse a doorbell badge the init does not mint");

    alignas(256) unsigned char g_block[K_BLOCK];

    struct kos_driver_instance instance_of(drv::Descriptor const& d)
    {
        struct kos_driver_instance in = {};
        in.name = "drvfake";
        in.mmio_base = d.expected_base;
        in.mmio_window = 0x40u;
        in.priority = K_PRIO;
        if (d.block_size != 0u)
        {
            in.block = g_block;
        }
        in.block_size = d.block_size;
        in.block_flags = d.block_flags;
        in.ceiling = K_PRIO + 1u;
        in.line_count = d.line_count;
        in.lines[0] = {40u, 2u};
        in.lines[1] = {41u, 3u};
        in.console = d.ep_posture == drv::KOS_DRV_EP_HANDOVER;
        return in;
    }

    // bring_up to the end of thread 0's body, which exits the entry thread: 1000 plus its code.
    int run(drv::Descriptor const& d, struct kos_driver_instance* in)
    {
        try
        {
            return drv::bring_up(d, in);
        }
        catch (kos_seam_exit const& e)
        {
            return 1000 + e.code;
        }
    }

    class DrvBringUp : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            kos_seam_reset();
            g_block_init_rc = 0;
        }
    };

    TEST_F(DrvBringUp, the_uart_shape_spawns_its_service_behind_the_barrier_then_runs_thread_0)
    {
        struct kos_driver_instance in = instance_of(k_uart);
        EXPECT_EQ(run(k_uart, &in), 1000);
        EXPECT_STREQ(kos_seam_trace(), "spawn50/p8 g3/1 g4/2 narrow1/1 narrow2/1 close3 close4 prio9 body0 exit0");
        EXPECT_EQ(kos_seam_spawn_arg(0), static_cast<void*>(&g_launch[1]))
            << "the service past the barrier starts in the readiness wait, not in its body";
        EXPECT_EQ(g_launch[1].arg, static_cast<void*>(g_block));
        EXPECT_EQ(g_launch[1].latch, drv::ready_latch(g_block, K_READY_OFF));
    }

    TEST_F(DrvBringUp, the_line_shape_hands_the_raiser_a_signal_copy_of_line_0)
    {
        struct kos_driver_instance in = instance_of(k_three);
        EXPECT_EQ(run(k_three, &in), 1000);
        EXPECT_STREQ(kos_seam_trace(),
                     "spawn50/p8 g4/1 spawn51/p8 g2/2 narrow1/1 narrow2/1 narrow3/1 close4 prio9 body0 exit0");
        EXPECT_EQ(kos_seam_spawn_arg(0), nullptr) << "a thread with no barrier starts in its body";
    }

    TEST_F(DrvBringUp, a_one_thread_driver_keeps_its_endpoint_and_spawns_nothing)
    {
        struct kos_driver_instance in = instance_of(k_one);
        EXPECT_EQ(run(k_one, &in), 1000);
        EXPECT_STREQ(kos_seam_trace(), "narrow1/1 body0 exit0");
    }

    TEST_F(DrvBringUp, an_entry_holding_nothing_hands_its_endpoint_on_and_closes_it)
    {
        struct kos_driver_instance in = instance_of(k_idle);
        EXPECT_EQ(run(k_idle, &in), 1000);
        EXPECT_STREQ(kos_seam_trace(), "spawn50/p8 g1/1 close1 body0 exit0");
    }

    TEST_F(DrvBringUp, every_thread_is_placed_on_the_declared_core)
    {
        struct kos_driver_instance in = instance_of(k_three);
        in.core_mask = 1u << 3;
        EXPECT_EQ(run(k_three, &in), 1000);
        EXPECT_STREQ(kos_seam_trace(),
                     "spawn50/p8 core8 g4/1 spawn51/p8 core8 g2/2 narrow1/1 narrow2/1 narrow3/1 close4 prio9 body0 "
                     "exit0");
    }

    TEST_F(DrvBringUp, a_hosted_instance_spawns_each_thread_that_waits_essential)
    {
        struct kos_driver_instance in = instance_of(k_three);
        kos_cap_t const hosted[] = {20, 21, 22, 23};
        in.hosted = hosted;
        EXPECT_EQ(run(k_three, &in), 0);
        EXPECT_STREQ(kos_seam_trace(),
                     "spawn50/p9 essential g20/1 g21/1 g22/1 spawn51/p8 essential g23/1 spawn52/p8 g21/2");
    }

    TEST_F(DrvBringUp, a_driver_task_spawns_no_thread_essential)
    {
        struct kos_driver_instance in = instance_of(k_three);
        EXPECT_EQ(run(k_three, &in), 1000);
        EXPECT_EQ(std::string(kos_seam_trace()).find("essential"), std::string::npos) << kos_seam_trace();
    }

    TEST_F(DrvBringUp, a_waiter_parked_before_the_producer_runs_once_it_is_published_and_woken)
    {
        g_seam.run_on_spawn = true;
        g_seam.publish_on_park = true;
        struct kos_driver_instance in = instance_of(k_uart);
        EXPECT_EQ(run(k_uart, &in), 1000);
        EXPECT_STREQ(kos_seam_trace(),
                     "spawn50/p8 g3/1 g4/2 park1000000 body1 narrow1/1 narrow2/1 close3 close4 prio9 body0 exit0");
    }

    TEST_F(DrvBringUp, a_waiter_whose_latch_is_never_published_ends_the_task_past_its_budget)
    {
        g_seam.run_on_spawn = true;
        struct kos_driver_instance in = instance_of(k_uart);
        EXPECT_EXIT(run(k_uart, &in), ::testing::KilledBySignal(SIGILL), "");
    }

    TEST_F(DrvBringUp, a_failed_spawn_ends_the_task_with_its_earlier_threads)
    {
        g_seam.spawn_fail_at = 2;
        struct kos_driver_instance in = instance_of(k_three);
        EXPECT_EXIT(run(k_three, &in), ::testing::KilledBySignal(SIGILL), "");
    }

    TEST_F(DrvBringUp, thread_0_refused_its_priority_ends_the_task)
    {
        g_seam.set_priority_rc = -KOS_EPERM;
        struct kos_driver_instance in = instance_of(k_one);
        EXPECT_EQ(run(k_one, &in), 1000) << "a thread 0 at its task's priority asks for none";
        struct kos_driver_instance three = instance_of(k_three);
        EXPECT_EXIT(run(k_three, &three), ::testing::KilledBySignal(SIGILL), "");
    }

    TEST_F(DrvBringUp, a_malformed_descriptor_is_refused_before_anything_is_spawned)
    {
        struct kos_driver_instance in = instance_of(k_uart);
        EXPECT_EQ(run(window_on(k_uart, 1), &in), -1);
        EXPECT_STREQ(kos_seam_trace(), "print print");
        EXPECT_TRUE(says(kos_seam_msg(), "not a well-formed driver shape"));
    }

    struct Mismatch
    {
        char const* what;
        void (*apply)(struct kos_driver_instance*);
        char const* says;
    };

    TEST_F(DrvBringUp, an_instance_that_is_not_this_driver_s_is_refused_before_anything_is_spawned)
    {
        Mismatch const arms[] = {
            {"a foreign window", [](struct kos_driver_instance* in) { in->mmio_base = K_BASE + 0x1000u; },
             "window is not this driver's block"},
            {"no ring block", [](struct kos_driver_instance* in) { in->block = nullptr; },
             "ring block is not this driver's"},
            {"a ring block of another type",
             [](struct kos_driver_instance* in) { in->block_flags = KOS_MEM_NOCACHE; },
             "ring block is not this driver's"},
            {"the console disowned", [](struct kos_driver_instance* in) { in->console = false; },
             "disagree on the console"},
            {"a line short", [](struct kos_driver_instance* in) { in->line_count = 0; }, "lines are not as many"},
        };
        for (Mismatch const& a : arms)
        {
            kos_seam_reset();
            struct kos_driver_instance in = instance_of(k_uart);
            a.apply(&in);
            EXPECT_EQ(run(k_uart, &in), -1) << a.what;
            EXPECT_STREQ(kos_seam_trace(), "print print") << a.what;
            EXPECT_TRUE(says(kos_seam_msg(), a.says)) << a.what;
        }
    }

    TEST_F(DrvBringUp, a_block_its_init_refuses_is_refused_before_anything_is_spawned)
    {
        g_block_init_rc = -1;
        struct kos_driver_instance in = instance_of(k_uart);
        EXPECT_EQ(run(k_uart, &in), -1);
        EXPECT_STREQ(kos_seam_trace(), "print print");
        EXPECT_TRUE(says(kos_seam_msg(), "block_init refused"));
    }

    TEST(DrvReady, a_publish_stores_the_latch_then_wakes_every_waiter)
    {
        kos_seam_reset();
        drv::Latch latch{0u};
        EXPECT_EQ(drv::ready_publish(&latch), 0);
        EXPECT_EQ(latch.load(), 1u);
        EXPECT_STREQ(kos_seam_trace(), "wake4294967295/0");
    }

    TEST(DrvReady, a_latch_already_set_is_not_waited_on)
    {
        kos_seam_reset();
        drv::Latch latch{1u};
        EXPECT_TRUE(drv::ready_wait(&latch));
        EXPECT_STREQ(kos_seam_trace(), "");
    }

    TEST(DrvReady, a_latch_never_set_is_waited_on_for_the_budget_then_refused)
    {
        kos_seam_reset();
        drv::Latch latch{0u};
        EXPECT_FALSE(drv::ready_wait(&latch));
        EXPECT_STREQ(kos_seam_trace(), "park1000000");
    }

    TEST(DrvReady, a_service_publish_stores_the_service_state_then_wakes_every_waiter)
    {
        kos_seam_reset();
        drv::Latch latch{drv::KOS_DRV_READY_DEVICE};
        EXPECT_EQ(drv::service_publish(&latch), 0);
        EXPECT_EQ(latch.load(), drv::KOS_DRV_READY_SERVICE);
        EXPECT_STREQ(kos_seam_trace(), "wake4294967295/0");
    }

    TEST(DrvReady, a_device_half_up_is_not_service_ready)
    {
        kos_seam_reset();
        drv::Latch latch{drv::KOS_DRV_READY_DEVICE};
        EXPECT_FALSE(drv::ready_wait_for(&latch, drv::KOS_DRV_READY_SERVICE));
        EXPECT_STREQ(kos_seam_trace(), "park1000000");
    }

    TEST(DrvReady, a_service_ready_latch_is_past_the_barrier_and_service_ready)
    {
        kos_seam_reset();
        drv::Latch latch{drv::KOS_DRV_READY_SERVICE};
        EXPECT_TRUE(drv::ready_wait(&latch));
        EXPECT_TRUE(drv::ready_wait_for(&latch, drv::KOS_DRV_READY_SERVICE));
        EXPECT_STREQ(kos_seam_trace(), "");
    }
}
