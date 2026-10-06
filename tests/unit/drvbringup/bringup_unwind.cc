// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Failure-path gate for kickos::driver::bring_up, over the recording seam in kos_seam.h.
//
// An arm checking an order compares the FULL ordered trace as one string, the only check on a
// console endpoint being narrowed before the diagnostic prints; a counter or a substring test
// drops it. Whether a narrow gives the kernel its console back is settled by
// tests/integration/check_sim_drvdeath.sh.

#include <kickos/sys/driver_service.h>

#include <kickos/sys/errno.h>

#include "kos_seam.h"

#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

namespace drv = kickos::driver;

namespace
{
    bool says(char const* msg, char const* needle)
    {
        return strstr(msg, needle) != nullptr;
    }

    // ---------------------------------------------------------------------------
    // A synthetic instance, not any chip's.

    constexpr uintptr_t K_BASE = 0x4000c000u;
    constexpr uint32_t K_BLOCK = 256u;
    constexpr uint16_t K_READY_OFF = 8u;

    void t_irq(void*) {}
    void t_worker(void*) {}
    void t_console(void*) {}

    int g_block_init_rc = 0;

    int block_init(void* blk, struct kos_driver_instance const*)
    {
        // The latch ADDRESS is published here; the spawn fake SETS it.
        g_seam.latch = reinterpret_cast<volatile uint32_t*>(
            static_cast<unsigned char*>(blk) + K_READY_OFF);
        *g_seam.latch = 0u;
        return g_block_init_rc;
    }

    // Two lines, so a partial claim is distinguishable from none: the arm that separates
    // `claimed` from `line_count` needs both.
    constexpr drv::Descriptor k_two = {
        .tag = "[drvfake] ",
        .expected_base = K_BASE,
        .block_size = K_BLOCK,
        .block_flags = 0,
        .ready_offset = K_READY_OFF,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .line_count = 2,
        .thread_count = 2,
        .barrier_after = 1,
        .lines = {{KOS_IRQ_LEVEL}, {KOS_IRQ_LEVEL}},
        .threads = {{.entry = t_irq,
                     .name = "drvirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 3,
                     .caps = {{drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT, 0}}},
                    {.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = block_init
    };

    // k_two with the block declared non-cacheable, and NOTHING else changed.
    constexpr drv::Descriptor k_two_nocache = {
        .tag = "[drvfakenc] ",
        .expected_base = K_BASE,
        .block_size = K_BLOCK,
        .block_flags = KOS_MEM_NOCACHE,
        .ready_offset = K_READY_OFF,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .line_count = 2,
        .thread_count = 2,
        .barrier_after = 1,
        .lines = {{KOS_IRQ_LEVEL}, {KOS_IRQ_LEVEL}},
        .threads = {{.entry = t_irq,
                     .name = "drvirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 3,
                     .caps = {{drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT, 0}}},
                    {.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = block_init
    };

    // Three threads leave TWO live peers at a third-spawn failure: what shows one group
    // kill ends both.
    constexpr drv::Descriptor k_three = {
        .tag = "[drvfake3] ",
        .expected_base = K_BASE,
        .block_size = K_BLOCK,
        .block_flags = 0,
        .ready_offset = K_READY_OFF,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .line_count = 2,
        .thread_count = 3,
        .barrier_after = 1,
        .lines = {{KOS_IRQ_LEVEL}, {KOS_IRQ_LEVEL}},
        .threads = {{.entry = t_irq,
                     .name = "drvirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 3,
                     .caps = {{drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT, 0}}},
                    {.entry = t_worker,
                     .name = "drvwork",
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 0,
                     .caps = {}},
                    {.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = block_init
    };

    // barrier_after == thread_count, the position only RETAIN admits: under HANDOVER, leg
    // L8 refuses this shape, so no console descriptor can reach it.
    constexpr drv::Descriptor k_tail_barrier = {
        .tag = "[drvtail] ",
        .expected_base = K_BASE,
        .block_size = K_BLOCK,
        .block_flags = 0,
        .ready_offset = K_READY_OFF,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_worker,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = block_init
    };

    // No block at all: the polled shape (k64uart, xmcuart, xmcssc, k64dspi, simcon). The
    // only way a driver's threads share no memory.
    constexpr drv::Descriptor k_blockless = {
        .tag = "[drvbare] ",
        .expected_base = 0,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_WINDOW,
                     .window_grant = true,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    // The same shape carrying a block no thread ever takes. L4 refuses it: the region lands
    // on every member for nobody to read.
    constexpr drv::Descriptor k_unread_block = {
        .tag = "[drvunread] ",
        .expected_base = 0,
        .block_size = K_BLOCK,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_WINDOW,
                     .window_grant = true,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = block_init
    };

    static_assert(drv::valid(k_two), "the two-thread gate descriptor is not a driver shape");
    static_assert(drv::valid(k_three), "the three-thread gate descriptor is not a driver shape");
    static_assert(drv::valid(k_tail_barrier),
                  "the tail-barrier gate descriptor is not a driver shape");
    static_assert(drv::valid(k_blockless), "the block-less gate descriptor is not a driver shape");
    static_assert(not drv::valid_l4(k_unread_block),
                  "L4 must refuse a block granted to the whole group that no thread reads");

    // NOT constexpr, so no leg runs at compile time: the subject is the run-time belt at the
    // top of bring_up, which keeps the spawn loop off the end of ThreadSet::t[].
    drv::Descriptor const k_overwide = {
        .tag = "[drvwide] ",
        .expected_base = K_BASE,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .line_count = 0,
        .thread_count = drv::KOS_DRV_THREADS_MAX + 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    // One window thread taking line 0's index, as a USIC channel routing its events does.
    constexpr drv::Descriptor k_routed = {
        .tag = "[drvroute] ",
        .expected_base = K_BASE,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .line_count = 1,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {{KOS_IRQ_EDGE}},
        .threads = {{.entry = t_irq,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_LINE0_INDEX,
                     .window_grant = true,
                     .cap_count = 3,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    // k_routed's thread on a descriptor that claims no line.
    constexpr drv::Descriptor k_routed_lineless = {
        .tag = "[drvroute0] ",
        .expected_base = 0,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_irq,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_LINE0_INDEX,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_routed), "the routed gate descriptor is not a driver shape");
    static_assert(not drv::valid_l2(k_routed_lineless), "L2 must refuse a line index of no line");
}

namespace
{
    constexpr kos_cap_t K_ENDPOINT = 7;
    constexpr kos_cap_t K_WATCH = 8;
    constexpr uint8_t K_PRIORITY = 8u;

    alignas(256) unsigned char g_ring[K_BLOCK];

    struct kos_driver_instance instance_of(drv::Descriptor const& d)
    {
        struct kos_driver_instance in = {};
        in.name = "drvfake";
        in.mmio_base = K_BASE;
        in.mmio_window = 0x40u;
        in.priority = K_PRIORITY;
        in.endpoint = K_ENDPOINT;
        in.watch = K_WATCH;
        in.task = 1234u;
        if (d.block_size != 0u)
        {
            in.block = g_ring;
            in.block_size = d.block_size;
            in.block_flags = d.block_flags;
        }
        in.line_count = d.line_count;
        in.console = d.ep_posture == drv::KOS_DRV_EP_HANDOVER;
        // What the tool emits for a driver at K_PRIORITY: it plus the highest offset.
        int8_t top = d.threads[0].prio_delta;
        for (uint8_t i = 1; i < d.thread_count; i++)
        {
            if (d.threads[i].prio_delta > top)
            {
                top = d.threads[i].prio_delta;
            }
        }
        in.ceiling = static_cast<uint8_t>(K_PRIORITY + top);
        for (uint8_t i = 0; i < d.line_count; i++)
        {
            in.lines[i] = {static_cast<uint16_t>(16u + i), i};
        }
        return in;
    }

    // The tokens a narrowed console endpoint leaves: SIGNAL, TRANSFER and HANDOUT.
    char const* const NARROW = "narrow7/14";
}

// The seam's arena and cap counter are static, so every arm must start from a reset.
class DrvInstance : public ::testing::Test
{
protected:
    void SetUp() override
    {
        kos_seam_reset();
        g_block_init_rc = 0;
    }
};

// A task owns ONE Domain, so a driver's block covers every member's region set whatever a
// given thread's arg says.
TEST_F(DrvInstance, a_block_reaches_the_group_even_where_a_thread_takes_no_block_argument)
{
    struct kos_driver_instance in = instance_of(k_three);
    g_seam.spawn_fail_at = 3;
    EXPECT_EQ(drv::bring_up(k_three, &in), -1);
    EXPECT_PRED2(says, kos_seam_trace(), "taskmem90")
        << "the block is the group's region, whatever any one thread's arg says";
}

// EXPECT_FALSE over the whole conjunction, so every OTHER leg must pass or the arm proves
// nothing: a declared line with no waiter would have L12 refuse this descriptor first.
TEST_F(DrvInstance, flags_on_a_block_that_does_not_exist_are_refused_by_the_validator)
{
    constexpr drv::Descriptor d = {
        .tag = "[drvfake] ",
        .expected_base = K_BASE,
        .block_size = 0,
        .block_flags = KOS_MEM_NOCACHE,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };
    EXPECT_FALSE(drv::valid(d)) << "flags with no block would be ignored in silence";
}

TEST_F(DrvInstance, a_driver_with_no_block_creates_a_group_that_shares_nothing)
{
    struct kos_driver_instance in = instance_of(k_blockless);
    EXPECT_EQ(drv::bring_up(k_blockless, &in), 0);
    EXPECT_EQ(kos_seam_trace(), std::string{"task90 grant90/8/0 watch90/8/7 pub7 spawn50 "} + NARROW + " probe")
        << "a task with no shared region";
}

TEST_F(DrvInstance, a_block_no_thread_reads_is_not_a_driver_shape)
{
    EXPECT_FALSE(drv::valid(k_unread_block))
        << "a group-wide grant with no reader is the widest ask a descriptor can make";
    EXPECT_TRUE(drv::valid(k_blockless)) << "and dropping the block is what makes it valid";
}

TEST_F(DrvInstance, a_foreign_window_is_refused_before_anything_is_made)
{
    struct kos_driver_instance in = instance_of(k_two);
    in.mmio_base = K_BASE + 0x1000u;
    EXPECT_EQ(drv::bring_up(k_two, &in), -1) << "a foreign window is refused";
    EXPECT_EQ(kos_seam_trace(), std::string{NARROW} + " print print") << "a foreign window makes nothing";
    EXPECT_PRED2(says, kos_seam_msg(), "window is not this driver's block");
}

// The group is created before any line.
TEST_F(DrvInstance, a_refused_task_takes_nothing_else)
{
    g_seam.task_create_fails = true;
    struct kos_driver_instance in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1) << "a refused task fails bring-up";
    EXPECT_EQ(kos_seam_trace(), std::string{"task! "} + NARROW + " print print")
        << "a refused task claims no line and spawns nobody";
    EXPECT_PRED2(says, kos_seam_msg(), "task_create failed") << "the diagnostic names the task";
}

// The readiness timeout at its real width: KOS_DRV_READY_WAIT_MAX sleeps of
// KOS_DRV_READY_WAIT_NS, about a second on silicon and free here.
TEST_F(DrvInstance, a_thread_that_never_reaches_its_loop)
{
    g_seam.latch_on_spawn = false;
    struct kos_driver_instance in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1) << "an unset latch fails bring-up";
    EXPECT_PRED2(says, kos_seam_trace(), "spawn50 sleep*1000 ")
        << "the readiness poll sleeps its full budget: " << kos_seam_trace();
    EXPECT_PRED2(says, kos_seam_msg(), "never reached its loop")
        << "the diagnostic names the readiness timeout";
}

// The barrier sits STRICTLY between the spawns: no poll before the first spawn.
TEST_F(DrvInstance, the_barrier_sits_between_the_spawns)
{
    g_seam.latch_on_spawn = false;
    struct kos_driver_instance in = instance_of(k_two);
    (void)drv::bring_up(k_two, &in);
    char const* const trace = kos_seam_trace();
    char const* const first_spawn = strstr(trace, "spawn50");
    char const* const first_sleep = strstr(trace, "sleep*");
    ASSERT_NE(first_spawn, nullptr) << "the trace has a first spawn: " << trace;
    ASSERT_NE(first_sleep, nullptr) << "the trace has a readiness sleep: " << trace;
    EXPECT_LT(first_spawn, first_sleep)
        << "the readiness poll runs after the first spawn, never before it: " << trace;
}

TEST_F(DrvInstance, an_unvetted_descriptor_is_refused_before_anything_is_taken)
{
    struct kos_driver_instance in = instance_of(k_blockless);
    EXPECT_EQ(drv::bring_up(k_overwide, &in), -1) << "a descriptor no leg vetted is refused";
    EXPECT_STREQ(kos_seam_trace(), "print print") << "the refusal makes nothing";
    EXPECT_PRED2(says, kos_seam_msg(), "well-formed driver shape")
        << "the diagnostic names the descriptor";
}

TEST_F(DrvInstance, a_bring_up_takes_the_init_s_block_and_endpoint_and_writes_its_task_back)
{
    struct kos_driver_instance in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), 0);
    EXPECT_EQ(kos_seam_trace(), std::string{"taskmem90 grant90/9/0 watch90/8/7 pub7 claim10 claim11"
                                            " note12 badge0 bind close13 badge1 bind close14 spawn50 spawn51"
                                            " close10 close11 close12 "} + NARROW + " probe")
        << "no alloc, no self-grant and no endpoint of its own; the console narrowed, never closed";
    EXPECT_EQ(in.task, 90u);
    EXPECT_STREQ(kos_seam_msg(), "");
}

TEST_F(DrvInstance, a_retained_endpoint_is_the_instance_s_and_is_not_narrowed)
{
    struct kos_driver_instance in = instance_of(k_tail_barrier);
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &in), 0);
    EXPECT_STREQ(kos_seam_trace(), "taskmem90 grant90/8/0 watch90/8/7 spawn50");
}

TEST_F(DrvInstance, the_grant_is_the_ceiling_the_table_carries)
{
    struct kos_driver_instance in = instance_of(k_three);
    in.core_mask = 1u << 2;
    in.ceiling = 17u;
    in.priority = 5u;
    EXPECT_EQ(drv::bring_up(k_three, &in), 0);
    EXPECT_PRED2(says, kos_seam_trace(), "grant90/17/4 ") << "the instance's ceiling, not one the bring-up derives: "
                                                          << kos_seam_trace();
}

TEST_F(DrvInstance, every_thread_runs_on_the_declared_core)
{
    struct kos_driver_instance in = instance_of(k_three);
    in.core_mask = 1u << 2;
    EXPECT_EQ(drv::bring_up(k_three, &in), 0);
    EXPECT_PRED2(says, kos_seam_trace(), "spawn50 core4 spawn51 core4 spawn52 core4 ")
        << "the line holder and the threads holding none alike: " << kos_seam_trace();
}

TEST_F(DrvInstance, a_failed_start_before_the_first_spawn_leaves_the_task_and_endpoint_to_the_init)
{
    g_seam.irq_claim_fail_at = 2;
    struct kos_driver_instance in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    EXPECT_EQ(kos_seam_trace(), std::string{"taskmem90 grant90/9/0 watch90/8/7 pub7 claim10 claim! close10 "}
                                    + NARROW + " print print")
        << "the claimed line closed, the console narrowed BEFORE the print, no tkill and no close of 7";
    EXPECT_EQ(in.task, 90u) << "the init slays what it was handed";
}

TEST_F(DrvInstance, a_failed_console_start_narrows_before_it_prints_at_every_step)
{
    struct Arm
    {
        char const* what;
        std::string trace;
    };
    std::vector<Arm> arms;

    kos_seam_reset();
    g_seam.sched_grant_fails = true;
    struct kos_driver_instance in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    arms.push_back({"sched_grant", kos_seam_trace()});

    kos_seam_reset();
    g_seam.watch_fails = true;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    arms.push_back({"watch", kos_seam_trace()});

    kos_seam_reset();
    g_seam.console_publish_fails = true;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    arms.push_back({"publish", kos_seam_trace()});

    kos_seam_reset();
    g_seam.spawn_fail_at = 2;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    arms.push_back({"spawn", kos_seam_trace()});

    kos_seam_reset();
    g_seam.latch_on_spawn = false;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    arms.push_back({"barrier", kos_seam_trace()});

    for (Arm const& arm : arms)
    {
        std::string const tail = std::string{NARROW} + " print print";
        ASSERT_GE(arm.trace.size(), tail.size()) << arm.what;
        EXPECT_EQ(arm.trace.substr(arm.trace.size() - tail.size()), tail) << arm.what << ": " << arm.trace;
        EXPECT_EQ(arm.trace.find("tkill"), std::string::npos) << arm.what;
        EXPECT_EQ(arm.trace.find("close7"), std::string::npos) << arm.what;
    }
}

// The init's endpoint, narrowed by the first start, is published again by the next one.
TEST_F(DrvInstance, a_restart_repeats_the_handover_and_a_failure_in_it_narrows_before_it_prints)
{
    struct kos_driver_instance in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), 0);
    std::string const first = kos_seam_trace();
    EXPECT_EQ(first, std::string{"taskmem90 grant90/9/0 watch90/8/7 pub7 claim10 claim11"
                                 " note12 badge0 bind close13 badge1 bind close14 spawn50 spawn51"
                                 " close10 close11 close12 "} + NARROW + " probe");

    kos_seam_reset();
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), 0);
    EXPECT_EQ(kos_seam_trace(), first) << "the restart publishes the same endpoint and hands over again";

    // The restart's receiver refused: its narrow is what gives the kernel its console back.
    kos_seam_reset();
    g_seam.spawn_fail_at = 2;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    EXPECT_EQ(kos_seam_trace(), std::string{"taskmem90 grant90/9/0 watch90/8/7 pub7 claim10 claim11"
                                            " note12 badge0 bind close13 badge1 bind close14 spawn50 spawn!"
                                            " close10 close11 close12 "} + NARROW + " print print")
        << "no tkill and no close of 7: the task and the endpoint stay the init's";
}

TEST_F(DrvInstance, a_handover_probe_that_is_not_taken_is_a_failed_start)
{
    g_seam.send_timed_rc = -KOS_EAGAIN;
    struct kos_driver_instance in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -KOS_EAGAIN);
    EXPECT_EQ(kos_seam_trace(), std::string{"taskmem90 grant90/9/0 watch90/8/7 pub7 claim10 claim11"
                                            " note12 badge0 bind close13 badge1 bind close14 spawn50 spawn51"
                                            " close10 close11 close12 "} + NARROW + " probe print print")
        << "narrowed, probed, reported; the init slays the task";
    EXPECT_PRED2(says, kos_seam_msg(), "probe was not taken");
}

TEST_F(DrvInstance, a_line_retiring_from_the_last_instance_is_claimed_again_within_the_bound)
{
    g_seam.irq_claim_retiring = drv::KOS_DRV_CLAIM_RETRIES;
    struct kos_driver_instance in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), 0);
    EXPECT_PRED2(says, kos_seam_trace(), "claim? sleep*1 claim?") << kos_seam_trace();

    kos_seam_reset();
    g_seam.irq_claim_retiring = drv::KOS_DRV_CLAIM_RETRIES + 1u;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1) << "past the bound, a failed start";
}

TEST_F(DrvInstance, an_instance_that_is_not_this_driver_s_is_refused_before_the_task)
{
    struct kos_driver_instance in = instance_of(k_two);

    in.line_count = 1u;
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    EXPECT_EQ(kos_seam_trace(), std::string{NARROW} + " print print") << "a line fewer than its roles";
    EXPECT_PRED2(says, kos_seam_msg(), "lines are not as many as");

    kos_seam_reset();
    in = instance_of(k_two);
    in.block_size = K_BLOCK * 2u;
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "ring block is not this driver's") << "a block of another size";

    kos_seam_reset();
    in = instance_of(k_two_nocache);
    in.block_flags = 0u;
    EXPECT_EQ(drv::bring_up(k_two_nocache, &in), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "ring block is not this driver's")
        << "a typed block the init's self-grant did not type";

    kos_seam_reset();
    in = instance_of(k_two);
    in.block_flags = KOS_MEM_NOCACHE;
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "ring block is not this driver's")
        << "an ordinary block the init's self-grant typed";
    EXPECT_EQ(in.task, KOS_TASK_NONE);
}

TEST_F(DrvInstance, a_typed_block_matching_its_descriptor_is_the_task_s)
{
    struct kos_driver_instance in = instance_of(k_two_nocache);
    EXPECT_EQ(drv::bring_up(k_two_nocache, &in), 0) << kos_seam_msg();
    EXPECT_PRED2(says, kos_seam_trace(), "taskmemnc90 ") << kos_seam_trace();
}

TEST_F(DrvInstance, the_composition_s_lines_are_claimed)
{
    struct kos_driver_instance in = instance_of(k_two);
    in.lines[0] = {40u, 3u};
    in.lines[1] = {41u, 4u};
    EXPECT_EQ(drv::bring_up(k_two, &in), 0) << kos_seam_msg();
    EXPECT_EQ(kos_seam_claimed_line(0), 40);
    EXPECT_EQ(kos_seam_claimed_line(1), 41);
    EXPECT_EQ(kos_seam_claimed_line(2), -1);
}

TEST_F(DrvInstance, line_0_s_index_reaches_the_thread_that_routes_its_device_onto_it)
{
    struct kos_driver_instance in = instance_of(k_routed);
    in.lines[0] = {40u, 5u};
    EXPECT_EQ(drv::bring_up(k_routed, &in), 0) << kos_seam_msg();
    EXPECT_EQ(kos_seam_claimed_line(0), 40);
    EXPECT_EQ(drv::line_index_of(kos_seam_spawn_arg(0)), 5u);
}

TEST_F(DrvInstance, a_block_the_descriptor_refuses_to_lay_out_is_a_failed_start_before_the_task)
{
    g_block_init_rc = -KOS_EINVAL;
    struct kos_driver_instance in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    EXPECT_EQ(kos_seam_trace(), std::string{NARROW} + " print print");
    EXPECT_EQ(in.task, KOS_TASK_NONE);
}

TEST_F(DrvInstance, a_failed_console_start_narrows_before_it_prints_at_every_claim_and_bind_step)
{
    struct Arm
    {
        char const* what;
        void (*arm)();
    };
    Arm const arms[] = {
        {"claim", []
         {
             g_seam.irq_claim_fail_at = 1;
         }},
        {"notify_create", []
         {
             g_seam.notify_create_fails = true;
         }},
        {"badge", []
         {
             g_seam.notify_badge_fails = true;
         }},
        {"bind", []
         {
             g_seam.irq_bind_notify_fails = true;
         }},
    };
    for (Arm const& a : arms)
    {
        kos_seam_reset();
        a.arm();
        struct kos_driver_instance in = instance_of(k_two);
        EXPECT_EQ(drv::bring_up(k_two, &in), -1) << a.what;
        std::string const trace = kos_seam_trace();
        std::string const tail = std::string{NARROW} + " print print";
        ASSERT_GE(trace.size(), tail.size()) << a.what;
        EXPECT_EQ(trace.substr(trace.size() - tail.size()), tail) << a.what << ": " << trace;
        EXPECT_EQ(trace.find("tkill"), std::string::npos) << a.what;
    }
}

TEST_F(DrvInstance, a_retained_endpoint_is_never_narrowed_on_a_failure)
{
    // Before any thread holds the endpoint, and with one live.
    g_seam.spawn_fail_at = 1;
    struct kos_driver_instance in = instance_of(k_tail_barrier);
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &in), -1);
    EXPECT_STREQ(kos_seam_trace(), "taskmem90 grant90/8/0 watch90/8/7 spawn! print print");

    kos_seam_reset();
    g_seam.latch_on_spawn = false;
    in = instance_of(k_tail_barrier);
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &in), -1);
    EXPECT_STREQ(kos_seam_trace(), "taskmem90 grant90/8/0 watch90/8/7 spawn50 sleep*1000 print print");
}

TEST_F(DrvInstance, a_table_and_a_descriptor_disagreeing_on_the_console_are_refused_before_the_task)
{
    struct kos_driver_instance in = instance_of(k_two);
    in.console = false;
    EXPECT_EQ(drv::bring_up(k_two, &in), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "disagree on the console");
    EXPECT_EQ(std::string{kos_seam_trace()}.find("task"), std::string::npos);

    kos_seam_reset();
    in = instance_of(k_tail_barrier);
    in.console = true;
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &in), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "disagree on the console");
}
