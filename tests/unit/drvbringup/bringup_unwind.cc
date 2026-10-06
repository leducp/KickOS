// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Failure-path gate for kickos::driver::bring_up and its unwind, over the recording seam
// in kos_seam.h.
//
// Every arm compares the FULL ordered trace as one string. That comparison is the only check
// on `unwind`'s three orderings (lines then endpoint, endpoint close before any cancel, both
// before the diagnostic print), and a counter or a substring test drops it.
//
// What the trace witnesses is the ORDER of the unwind calls. Whether kos_thread_kill is
// honoured (cancellation is cooperative, taken inside a notification wait, and no spawned
// thread runs here) and whether a close reclaims the console are settled by
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

    int block_init(void* blk, struct kos_service_cfg const*)
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
        .svc_kind = KOS_SVC_SPI,
        .line_count = 1,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {{21, KOS_IRQ_EDGE, 2}},
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
        .svc_kind = KOS_SVC_SPI,
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

    // k_routed with no index stated for its line, which a service list would hand the thread.
    constexpr drv::Descriptor k_routed_unindexed = {
        .tag = "[drvroute1] ",
        .expected_base = K_BASE,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .svc_kind = KOS_SVC_SPI,
        .line_count = 1,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {{21, KOS_IRQ_EDGE}},
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

    static_assert(drv::valid(k_routed), "the routed gate descriptor is not a driver shape");
    static_assert(not drv::valid_l2(k_routed_unindexed),
                  "L2 must refuse a line index a service list leaves unstated");
    static_assert(not drv::valid_l2(k_routed_lineless), "L2 must refuse a line index of no line");

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
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12"
                 " note13 badge0 bind close14 badge1 bind close15 spawn50 spawn51"
                 " close11 close12 close13 close10 probe")
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
                 "alloc grantnc taskmemnc90 ep10 pub10 claim11 claim12"
                 " note13 badge0 bind close14 badge1 bind close15 spawn50 spawn51"
                 " close11 close12 close13 close10 probe")
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
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
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

// A refusal must leave NO trace, one arm per leg of the guard.
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
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12"
                 " note13 badge0 bind close14 badge1 bind close15 spawn!"
                 " close11 close12 close13 close10 tkill90 print print")
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
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12"
                 " note13 badge0 bind close14 badge1 bind close15 spawn50 spawn!"
                 " close11 close12 close13 close10 tkill90 print print")
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
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12"
                 " note13 badge0 bind close14 badge1 bind close15 spawn50 spawn51 spawn!"
                 " close11 close12 close13 close10 tkill90 print print")
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
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12"
                 " note13 badge0 bind close14 badge1 bind close15 spawn50 sleep*1000"
                 " close11 close12 close13 close10 tkill90 print print")
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
    g_seam.send_timed_rc = -KOS_ECONNREFUSED;
    struct kos_service_cfg const cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -KOS_ECONNREFUSED)
        << "a refused probe returns its refusal unchanged";
    EXPECT_STREQ(kos_seam_trace(),
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12"
                 " note13 badge0 bind close14 badge1 bind close15 spawn50 spawn51"
                 " close11 close12 close13 close10 probe tkill90 print print")
        << "a refused probe ends the whole group, after the close and after the probe";
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
                 "alloc grant taskmem90 ep10 pub10 claim11 claim12"
                 " note13 badge0 bind close14 badge1 bind close15 spawn50 spawn51"
                 " close11 close12 close13 close10 probe")
        << "a timed-out probe leaves the group alone and prints nothing";
    EXPECT_STREQ(kos_seam_msg(), "") << "a timed-out probe prints no diagnostic";
}

// ---------------------------------------------------------------------------
// Given an instance (docs/design-m10-target.md, section 2): the init's ring block and endpoint,
// the task written back, nothing of the init's closed on a failure.

namespace
{
    constexpr kos_cap_t K_ENDPOINT = 7;
    constexpr kos_cap_t K_WATCH = 8;

    alignas(256) unsigned char g_ring[K_BLOCK];

    struct kos_driver_instance instance_of(drv::Descriptor const& d)
    {
        struct kos_driver_instance in = {};
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
        for (uint8_t i = 0; i < d.line_count; i++)
        {
            in.lines[i] = {static_cast<uint16_t>(d.lines[i].number), d.lines[i].index};
        }
        return in;
    }

    struct kos_service_cfg cfg_with(struct kos_driver_instance* in)
    {
        // The init fills no kind: the bring-up takes it from the descriptor.
        struct kos_service_cfg cfg = cfg_of(0xFFu, K_BASE);
        cfg.instance = in;
        return cfg;
    }

    // The tokens a narrowed console endpoint leaves: SIGNAL, TRANSFER and HANDOUT.
    char const* const NARROW = "narrow7/14";
}

class DrvInstance : public DrvBringup
{
};

TEST_F(DrvInstance, a_bring_up_takes_the_init_s_block_and_endpoint_and_writes_its_task_back)
{
    struct kos_driver_instance in = instance_of(k_two);
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), 0);
    EXPECT_EQ(kos_seam_trace(), std::string{"taskmem90 grant90/9/0 watch90/8/7 pub7 claim10 claim11"
                                            " note12 badge0 bind close13 badge1 bind close14 spawn50 spawn51"
                                            " close10 close11 close12 "} + NARROW + " probe")
        << "no alloc, no self-grant and no endpoint of its own; the console narrowed, never closed";
    EXPECT_EQ(in.task, 90u);
    EXPECT_STREQ(kos_seam_msg(), "");
}

TEST_F(DrvInstance, a_retained_endpoint_is_the_instance_s_and_out_ep_is_left_alone)
{
    struct kos_driver_instance in = instance_of(k_tail_barrier);
    struct kos_service_cfg const cfg = cfg_with(&in);
    kos_cap_t out = 99u;
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &cfg, &out), 0);
    EXPECT_STREQ(kos_seam_trace(), "taskmem90 grant90/8/0 watch90/8/7 spawn50");
    EXPECT_EQ(out, 99u);
}

TEST_F(DrvInstance, a_retained_endpoint_takes_no_out_ep_and_is_not_narrowed)
{
    struct kos_driver_instance in = instance_of(k_tail_barrier);
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &cfg, nullptr), 0);
    EXPECT_STREQ(kos_seam_trace(), "taskmem90 grant90/8/0 watch90/8/7 spawn50");
}

TEST_F(DrvInstance, the_ceiling_is_the_priority_plus_the_highest_thread_offset)
{
    struct kos_driver_instance in = instance_of(k_three);
    in.core_mask = 1u << 2;
    struct kos_service_cfg cfg = cfg_with(&in);
    cfg.prio = 5u;
    EXPECT_EQ(drv::bring_up(k_three, &cfg, nullptr), 0);
    EXPECT_PRED2(says, kos_seam_trace(), "grant90/6/4 ") << kos_seam_trace();
}

TEST_F(DrvInstance, every_thread_runs_on_the_declared_core)
{
    struct kos_driver_instance in = instance_of(k_three);
    in.core_mask = 1u << 2;
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_three, &cfg, nullptr), 0);
    EXPECT_PRED2(says, kos_seam_trace(), "spawn50 core4 spawn51 core4 spawn52 core4 ")
        << "the line holder and the threads holding none alike: " << kos_seam_trace();
}

TEST_F(DrvInstance, a_failed_start_before_the_first_spawn_leaves_the_task_and_endpoint_to_the_init)
{
    g_seam.irq_claim_fail_at = 2;
    struct kos_driver_instance in = instance_of(k_two);
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
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
    struct kos_service_cfg cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    arms.push_back({"sched_grant", kos_seam_trace()});

    kos_seam_reset();
    g_seam.watch_fails = true;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    arms.push_back({"watch", kos_seam_trace()});

    kos_seam_reset();
    g_seam.console_publish_fails = true;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    arms.push_back({"publish", kos_seam_trace()});

    kos_seam_reset();
    g_seam.spawn_fail_at = 2;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    arms.push_back({"spawn", kos_seam_trace()});

    kos_seam_reset();
    g_seam.latch_on_spawn = false;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
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
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), 0);
    std::string const first = kos_seam_trace();
    EXPECT_EQ(first, std::string{"taskmem90 grant90/9/0 watch90/8/7 pub7 claim10 claim11"
                                 " note12 badge0 bind close13 badge1 bind close14 spawn50 spawn51"
                                 " close10 close11 close12 "} + NARROW + " probe");

    kos_seam_reset();
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), 0);
    EXPECT_EQ(kos_seam_trace(), first) << "the restart publishes the same endpoint and hands over again";

    // The restart's receiver refused: its narrow is what gives the kernel its console back.
    kos_seam_reset();
    g_seam.spawn_fail_at = 2;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    EXPECT_EQ(kos_seam_trace(), std::string{"taskmem90 grant90/9/0 watch90/8/7 pub7 claim10 claim11"
                                            " note12 badge0 bind close13 badge1 bind close14 spawn50 spawn!"
                                            " close10 close11 close12 "} + NARROW + " print print")
        << "no tkill and no close of 7: the task and the endpoint stay the init's";
}

TEST_F(DrvInstance, a_handover_probe_that_is_not_taken_is_a_failed_start)
{
    g_seam.send_timed_rc = -KOS_EAGAIN;
    struct kos_driver_instance in = instance_of(k_two);
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -KOS_EAGAIN);
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
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), 0);
    EXPECT_PRED2(says, kos_seam_trace(), "claim? sleep*1 claim?") << kos_seam_trace();

    kos_seam_reset();
    g_seam.irq_claim_retiring = drv::KOS_DRV_CLAIM_RETRIES + 1u;
    in = instance_of(k_two);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1) << "past the bound, a failed start";
}

TEST_F(DrvInstance, an_instance_that_is_not_this_driver_s_is_refused_before_the_task)
{
    struct kos_driver_instance in = instance_of(k_two);
    struct kos_service_cfg const cfg = cfg_with(&in);

    in.line_count = 1u;
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    EXPECT_EQ(kos_seam_trace(), std::string{NARROW} + " print print") << "a line fewer than its roles";
    EXPECT_PRED2(says, kos_seam_msg(), "lines are not as many as");

    kos_seam_reset();
    in = instance_of(k_two);
    in.block_size = K_BLOCK * 2u;
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "ring block is not this driver's") << "a block of another size";

    kos_seam_reset();
    in = instance_of(k_two_nocache);
    in.block_flags = 0u;
    EXPECT_EQ(drv::bring_up(k_two_nocache, &cfg, nullptr), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "ring block is not this driver's")
        << "a typed block the init's self-grant did not type";

    kos_seam_reset();
    in = instance_of(k_two);
    in.block_flags = KOS_MEM_NOCACHE;
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "ring block is not this driver's")
        << "an ordinary block the init's self-grant typed";
    EXPECT_EQ(in.task, KOS_TASK_NONE);
}

TEST_F(DrvInstance, a_typed_block_matching_its_descriptor_is_the_task_s)
{
    struct kos_driver_instance in = instance_of(k_two_nocache);
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two_nocache, &cfg, nullptr), 0) << kos_seam_msg();
    EXPECT_PRED2(says, kos_seam_trace(), "taskmemnc90 ") << kos_seam_trace();
}

TEST_F(DrvInstance, the_composition_s_lines_are_claimed_whatever_the_descriptor_numbers)
{
    struct kos_driver_instance in = instance_of(k_two);
    in.lines[0] = {40u, 3u};
    in.lines[1] = {41u, 4u};
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), 0) << kos_seam_msg();
    EXPECT_EQ(kos_seam_claimed_line(0), 40);
    EXPECT_EQ(kos_seam_claimed_line(1), 41);
    EXPECT_EQ(kos_seam_claimed_line(2), -1);
}

TEST_F(DrvInstance, line_0_s_index_reaches_the_thread_that_routes_its_device_onto_it)
{
    struct kos_driver_instance in = instance_of(k_routed);
    in.lines[0] = {40u, 5u};
    struct kos_service_cfg cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_routed, &cfg, nullptr), 0) << kos_seam_msg();
    EXPECT_EQ(kos_seam_claimed_line(0), 40);
    EXPECT_EQ(drv::line_index_of(drv::thread_start(kos_seam_spawn_arg(0))), 5u);

    kos_seam_reset();
    cfg = cfg_of(KOS_SVC_SPI, K_BASE);
    kos_cap_t out = KOS_CAP_NONE;
    EXPECT_EQ(drv::bring_up(k_routed, &cfg, &out), 0) << kos_seam_msg();
    EXPECT_EQ(kos_seam_claimed_line(0), 21);
    EXPECT_EQ(drv::line_index_of(drv::thread_start(kos_seam_spawn_arg(0))), 2u)
        << "on a service list, the descriptor's own index";
}

TEST_F(DrvInstance, a_block_the_descriptor_refuses_to_lay_out_is_a_failed_start_before_the_task)
{
    g_block_init_rc = -KOS_EINVAL;
    struct kos_driver_instance in = instance_of(k_two);
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
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
        struct kos_service_cfg const cfg = cfg_with(&in);
        EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1) << a.what;
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
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &cfg, nullptr), -1);
    EXPECT_STREQ(kos_seam_trace(), "taskmem90 grant90/8/0 watch90/8/7 spawn! print print");

    kos_seam_reset();
    g_seam.latch_on_spawn = false;
    in = instance_of(k_tail_barrier);
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &cfg, nullptr), -1);
    EXPECT_STREQ(kos_seam_trace(), "taskmem90 grant90/8/0 watch90/8/7 spawn50 sleep*1000 print print");
}

TEST_F(DrvInstance, a_table_and_a_descriptor_disagreeing_on_the_console_are_refused_before_the_task)
{
    struct kos_driver_instance in = instance_of(k_two);
    in.console = false;
    struct kos_service_cfg cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "disagree on the console");
    EXPECT_EQ(std::string{kos_seam_trace()}.find("task"), std::string::npos);

    kos_seam_reset();
    in = instance_of(k_tail_barrier);
    in.console = true;
    cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_tail_barrier, &cfg, nullptr), -1);
    EXPECT_PRED2(says, kos_seam_msg(), "disagree on the console");
}

TEST_F(DrvBringup, a_cfg_whose_reserved_bytes_are_set_is_refused)
{
    struct kos_service_cfg cfg = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    cfg.rsv[3] = 1u;
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    EXPECT_STREQ(kos_seam_trace(), "print print");
    EXPECT_PRED2(says, kos_seam_msg(), "reserved bytes");
}

TEST_F(DrvBringup, an_instance_whose_block_is_odd_is_refused_before_anything_is_made)
{
    struct kos_driver_instance in = instance_of(k_two);
    in.block = static_cast<unsigned char*>(in.block) + 1;
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), -1);
    EXPECT_STREQ(kos_seam_trace(), "narrow7/14 print print");
    EXPECT_PRED2(says, kos_seam_msg(), "cannot carry the posture");
}

TEST_F(DrvBringup, a_failing_thread_traps_under_the_init_and_returns_on_a_service_list)
{
    // Each thread records the posture its spawn's arg carries, as its entry's thread_start does.
    struct kos_service_cfg const listed = cfg_of(KOS_SVC_CONSOLE, K_BASE);
    EXPECT_EQ(drv::bring_up(k_two, &listed, nullptr), 0);
    EXPECT_EQ(drv::thread_start(kos_seam_spawn_arg(1)), kos_seam_spawn_arg(1));
    drv::trap_under_init();

    kos_seam_reset();
    struct kos_driver_instance in = instance_of(k_two);
    struct kos_service_cfg const cfg = cfg_with(&in);
    EXPECT_EQ(drv::bring_up(k_two, &cfg, nullptr), 0);
    EXPECT_EQ(drv::thread_start(kos_seam_spawn_arg(1)), in.block);
    EXPECT_DEATH(drv::trap_under_init(), "");

    kos_seam_reset();
    EXPECT_EQ(drv::bring_up(k_two, &listed, nullptr), 0);
    (void)drv::thread_start(kos_seam_spawn_arg(1));
    drv::trap_under_init();
}
