// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Failure-path gate for kickos::driver::bring_up and its unwind, over the recording seam
// in kos_seam.h.
//
// Every arm compares the FULL ordered trace as one string. Counters, substring tests or
// per-token assertions would drop the only check on `unwind`'s three orderings: lines then
// endpoint, endpoint close before any cancel, both before the diagnostic print.
//
// NOT witnessed here: that kos_thread_kill was honoured (cancellation is cooperative and
// taken only inside kos_irq_wait, and no spawned thread runs here; see
// tests/integration/check_sim_drvdeath.sh), nor that a close reclaims the console.

#include <kickos/sys/driver_service.h>

#include <kickos/sys/errno.h>

#include "kos_seam.h"

#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

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

    int block_init(void* blk, struct kos_service_cfg const*)
    {
        // The latch ADDRESS is published here; the spawn fake SETS it, as no child runs.
        g_seam.latch = reinterpret_cast<volatile uint32_t*>(
            static_cast<unsigned char*>(blk) + K_READY_OFF);
        *g_seam.latch = 0u;
        return g_block_init_rc;
    }

    // Two lines: with one, a partial claim is indistinguishable from none and the arm that
    // separates `claimed` from `line_count` dies with it.
    constexpr drv::Descriptor k_two = {
        .tag = "[drvfake] ",
        .expected_base = K_BASE,
        .block_size = K_BLOCK,
        .block_flags = 0,
        .ready_offset = K_READY_OFF,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 2,
        .thread_count = 2,
        .barrier_after = 1,
        .lines = {{16, KOS_IRQ_LEVEL}, {17, KOS_IRQ_LEVEL}},
        .threads = {{.entry = t_irq,
                     .name = "drvirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT}}},
                    {.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
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
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 2,
        .thread_count = 2,
        .barrier_after = 1,
        .lines = {{16, KOS_IRQ_LEVEL}, {17, KOS_IRQ_LEVEL}},
        .threads = {{.entry = t_irq,
                     .name = "drvirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT}}},
                    {.entry = t_console,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
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
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 2,
        .thread_count = 3,
        .barrier_after = 1,
        .lines = {{16, KOS_IRQ_LEVEL}, {17, KOS_IRQ_LEVEL}},
        .threads = {{.entry = t_irq,
                     .name = "drvirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT}}},
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
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
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
        .svc_kind = KOS_SVC_SPI,
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
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
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
        .svc_kind = KOS_SVC_CONSOLE,
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
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
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
        .svc_kind = KOS_SVC_CONSOLE,
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
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
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
        .svc_kind = KOS_SVC_CONSOLE,
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
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
        .block_init = nullptr
    };

    struct kos_service_cfg cfg_of(uint8_t kind, uintptr_t base)
    {
        struct kos_service_cfg cfg = {};
        cfg.name = "drvfake";
        cfg.mmio_base = base;
        cfg.mmio_window = 0x40u;
        cfg.hz = 115200u;
        cfg.prio = 8u;
        cfg.kind = kind;
        return cfg;
    }
}

// The seam's arena and cap counter are static, so every arm must start from a reset.
class DrvBringup : public ::testing::Test
{
protected:
    void SetUp() override
    {
        kos_seam_reset();
        g_block_init_rc = 0;
    }
};

// THE POSITIVE CONTROL, and it may not be removed.
TEST_F(DrvBringup, a_complete_bring_up_touches_no_unwind)
{
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), 0) << "a complete bring-up returns 0";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12 spawn50 spawn51"
                 " close11 close12 close10 probe")
        << "a complete bring-up makes the group, claims, spawns, drops its lines and probes";
    EXPECT_STREQ(kos_seam_msg(), "") << "a complete bring-up prints no diagnostic";
}

// A task owns ONE Domain, so a driver's block covers every member's region set whatever a
// given thread's arg says.
TEST_F(DrvBringup, a_block_reaches_the_group_even_where_a_thread_takes_no_block_argument)
{
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    g_seam.spawn_fail_at = 3;
    EXPECT_EQ(drv::bring_up(k_three, &cfg, nullptr), -1);
    EXPECT_PRED2(says, kos_seam_trace(), "taskmem90")
        << "the block is the group's region, whatever any one thread's arg says";
}

// BOTH mappings or neither. The bring-up WRITES the block through its own self-grant before
// handing it to the task, so a type on the task grant alone leaves those initialising writes
// sitting in dirty lines under whatever bus master then reads the block.
TEST_F(DrvBringup, a_non_cacheable_block_carries_the_flag_on_BOTH_of_its_grants)
{
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two_nocache, &cfg, nullptr), 0);
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grantnc taskmemnc90 ep10 pub10 claim11 claim12 spawn50 spawn51"
                 " close11 close12 close10 probe")
        << "the self-grant and the task grant must both carry KOS_MEM_NOCACHE";
}

// The negative control for the arm above: the tokens must READ block_flags, not be constants.
TEST_F(DrvBringup, an_ordinary_block_asks_for_no_memory_type)
{
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), 0);
    EXPECT_PRED2(says, kos_seam_trace(), "alloc grant taskmem90")
        << "a cacheable block must reach neither grant with a memory type";
}

// EXPECT_FALSE over the whole conjunction, so every OTHER leg must pass or the arm proves
// nothing: a declared line with no waiter would have L12 refuse this descriptor first.
TEST_F(DrvBringup, flags_on_a_block_that_does_not_exist_are_refused_by_the_validator)
{
    constexpr drv::Descriptor d = {
        .tag = "[drvfake] ",
        .expected_base = K_BASE,
        .block_size = 0,
        .block_flags = KOS_MEM_NOCACHE,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
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
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
        .block_init = nullptr
    };
    EXPECT_FALSE(drv::valid(d)) << "flags with no block would be ignored in silence";
}

TEST_F(DrvBringup, a_driver_with_no_block_creates_a_group_that_shares_nothing)
{
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_blockless, &cfg, nullptr), 0);
    EXPECT_STREQ(kos_seam_trace(), "task90 ep10 pub10 spawn50 close10 probe")
        << "no alloc, no grant, and a task with no shared region";
}

TEST_F(DrvBringup, a_block_no_thread_reads_is_not_a_driver_shape)
{
    EXPECT_FALSE(drv::valid(k_unread_block))
        << "a group-wide grant with no reader is the widest ask a descriptor can make";
    EXPECT_TRUE(drv::valid(k_blockless)) << "and dropping the block is what makes it valid";
}

// A refusal must leave NO trace: no arena block, no endpoint, no publish. One arm per leg
// of the guard.
TEST_F(DrvBringup, a_wrong_kind_cfg_has_no_effect)
{
    struct kos_service_cfg const wrong = cfg_of(KOS_SVC_SPI, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &wrong, nullptr), -1) << "a wrong-kind cfg is refused";
    EXPECT_STREQ(kos_seam_trace(), "print print") << "a wrong-kind cfg allocates nothing";
}

TEST_F(DrvBringup, a_foreign_mmio_base_has_no_effect)
{
    struct kos_service_cfg const elsewhere = cfg_of(KOS_SVC_CONSOLE, K_BASE + 0x1000u);
    EXPECT_EQ(drv::bring_up(k_two, &elsewhere, nullptr), -1)
        << "a foreign mmio_base is refused";
    EXPECT_STREQ(kos_seam_trace(), "print print") << "a foreign mmio_base allocates nothing";
}

TEST_F(DrvBringup, an_out_ep_under_handover_has_no_effect)
{
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    kos_cap_t out = KOS_CAP_NONE;
    EXPECT_EQ(drv::bring_up(k_two, &cfg, &out), -1) << "an out_ep under HANDOVER is refused";
    EXPECT_STREQ(kos_seam_trace(), "print print") << "a posture mismatch allocates nothing";
}

// The first claim fails, so `claimed` is 0 and NOTHING may be closed but the endpoint.
// Closing line[0] here would close KOS_CAP_NONE.
TEST_F(DrvBringup, the_first_irq_claim_fails)
{
    g_seam.irq_claim_fail_at = 1;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1) << "a refused first line fails bring-up";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub10 claim! close10 tkill90 print print")
        << "a refused first line closes the endpoint and no line";
    EXPECT_PRED2(says, kos_seam_msg(), "irq_claim failed")
        << "the diagnostic names the failed claim";
}

// EXACTLY the first line is closed: the arm that separates `claimed` from `line_count`.
TEST_F(DrvBringup, the_second_irq_claim_fails)
{
    g_seam.irq_claim_fail_at = 2;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1)
        << "a refused second line fails bring-up";
    EXPECT_STREQ(kos_seam_trace(), "alloc grant taskmem90 ep10 pub10 claim11 claim!"
                                   " close11 close10 tkill90 print print")
        << "a refused second line closes the one line it did claim, then the endpoint";
}

TEST_F(DrvBringup, the_first_spawn_fails)
{
    g_seam.spawn_fail_at = 1;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1)
        << "a refused first spawn fails bring-up";
    EXPECT_STREQ(kos_seam_trace(), "alloc grant taskmem90 ep10 pub10 claim11 claim12 spawn!"
                                   " close11 close12 close10 tkill90 print print")
        << "a refused first spawn closes both lines and cancels nobody";
    EXPECT_PRED2(says, kos_seam_msg(), "spawn failed")
        << "the diagnostic names the failed spawn";
}

// One live peer at the failure: the close-before-cancel ordering becomes observable.
TEST_F(DrvBringup, a_later_spawn_fails_and_the_peer_is_cancelled)
{
    g_seam.spawn_fail_at = 2;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1)
        << "a refused later spawn fails bring-up";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12 spawn50 spawn!"
                 " close11 close12 close10 tkill90 print print")
        << "a refused later spawn closes the endpoint BEFORE ending the group";
}

// TWO live peers, ended by ONE call that names no thread.
TEST_F(DrvBringup, two_live_peers_are_ended_by_one_group_kill)
{
    g_seam.spawn_fail_at = 3;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_three, &cfg, nullptr), -1)
        << "a refused third spawn fails bring-up";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12 spawn50 spawn51 spawn!"
                 " close11 close12 close10 tkill90 print print")
        << "two live peers are ended by one kill naming the task, not the threads";
}

// The group is created BEFORE the endpoint and before any line.
TEST_F(DrvBringup, a_refused_task_takes_nothing_else)
{
    g_seam.task_create_fails = true;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1) << "a refused task fails bring-up";
    EXPECT_STREQ(kos_seam_trace(), "alloc grant task! print print")
        << "a refused task creates no endpoint, claims no line and spawns nobody";
    EXPECT_PRED2(says, kos_seam_msg(), "task_create failed")
        << "the diagnostic names the task";
}

// The readiness timeout at its real width: KOS_DRV_READY_WAIT_MAX sleeps of
// KOS_DRV_READY_WAIT_NS, about a second on silicon and free here.
TEST_F(DrvBringup, a_thread_that_never_reaches_its_loop)
{
    g_seam.latch_on_spawn = false;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1) << "an unset latch fails bring-up";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12 spawn50 sleep*1000"
                 " close11 close12 close10 tkill90 print print")
        << "the readiness poll sleeps its full budget, then ends the group";
    EXPECT_PRED2(says, kos_seam_msg(), "never reached its loop")
        << "the diagnostic names the readiness timeout";
}

// The barrier sits STRICTLY between the spawns: no poll before the first spawn.
TEST_F(DrvBringup, the_barrier_sits_between_the_spawns)
{
    g_seam.latch_on_spawn = false;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    (void)drv::bring_up(k_two, &cfg, nullptr);
    char const* const trace = kos_seam_trace();
    char const* const first_spawn = strstr(trace, "spawn50");
    char const* const first_sleep = strstr(trace, "sleep*");
    ASSERT_NE(first_spawn, nullptr) << "the trace has a first spawn: " << trace;
    ASSERT_NE(first_sleep, nullptr) << "the trace has a readiness sleep: " << trace;
    EXPECT_LT(first_spawn, first_sleep)
        << "the readiness poll runs after the first spawn, never before it: " << trace;
}

// The LAST barrier position, which only a one-thread service uses. The rc alone cannot tell
// "polled after the spawn" from "never polled"; the timeout arm below is the discriminating
// witness, its sleep token being reachable only after spawn50.
TEST_F(DrvBringup, the_barrier_can_sit_after_the_last_spawn)
{
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_SPI, K_BASE);
    kos_cap_t out = KOS_CAP_NONE;
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &cfg, &out), 0)
        << "a latch polled after the only spawn completes bring-up";
    EXPECT_NE(out, KOS_CAP_NONE) << "the retained endpoint reaches the caller";
    EXPECT_STREQ(kos_seam_trace(), "alloc grant taskmem90 ep10 spawn50")
        << "the poll adds no sleep when the latch is set";
}

TEST_F(DrvBringup, the_barrier_after_the_last_spawn_times_out)
{
    g_seam.latch_on_spawn = false;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_SPI, K_BASE);
    kos_cap_t out = KOS_CAP_NONE;
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &cfg, &out), -1)
        << "an unset latch at the last position fails bring-up";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 spawn50 sleep*1000 close10 tkill90"
                                   " print print")
        << "the poll runs AFTER the only spawn, then unwinds it";
    EXPECT_PRED2(says, kos_seam_msg(), "never reached its loop")
        << "the diagnostic names the readiness timeout";
}

TEST_F(DrvBringup, an_unvetted_descriptor_is_refused_before_anything_is_taken)
{
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_overwide, &cfg, nullptr), -1)
        << "a descriptor no leg vetted is refused";
    EXPECT_STREQ(kos_seam_trace(), "print print")
        << "the refusal allocates nothing and spawns nothing";
    EXPECT_PRED2(says, kos_seam_msg(), "well-formed driver shape")
        << "the diagnostic names the descriptor";
}

// block_init refuses: the block is allocated and granted, and there is no endpoint yet
// to close. A close here would close KOS_CAP_NONE.
TEST_F(DrvBringup, block_init_refuses_the_cfg)
{
    g_block_init_rc = -KOS_EINVAL;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1)
        << "a refused block_init fails bring-up";
    EXPECT_STREQ(kos_seam_trace(), "alloc grant print print")
        << "a refused block_init closes nothing";
}

TEST_F(DrvBringup, the_publish_fails)
{
    g_seam.console_publish_fails = true;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1) << "a refused publish fails bring-up";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub! close10 tkill90 print print")
        << "a refused publish closes the endpoint it could not publish";
}

// This arm and the next differ only in their SIDE EFFECTS: both return a negative code.
TEST_F(DrvBringup, the_handover_probe_reports_a_dead_driver)
{
    g_seam.send_timed_rc = -KOS_EPIPE;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -KOS_EPIPE)
        << "an EPIPE probe returns EPIPE unchanged";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12 spawn50 spawn51"
                 " close11 close12 close10 probe tkill90 print print")
        << "an EPIPE probe ends the whole group, after the close and after the probe";
    EXPECT_PRED2(says, kos_seam_msg(), "died during bring-up")
        << "the diagnostic names the dead thread";
}

TEST_F(DrvBringup, a_timed_out_handover_probe_cancels_nothing)
{
    g_seam.send_timed_rc = -KOS_ETIMEDOUT;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -KOS_ETIMEDOUT)
        << "a timed-out probe returns ETIMEDOUT unchanged";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12 spawn50 spawn51"
                 " close11 close12 close10 probe")
        << "a timed-out probe leaves the group alone and prints nothing";
    EXPECT_STREQ(kos_seam_msg(), "") << "a timed-out probe prints no diagnostic";
}
