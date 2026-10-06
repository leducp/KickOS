// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kickos::driver::bring_up built for more than one kernel core, over the recording seam: where
// it pins itself for the claims and where it places each thread.

#include <kickos/sys/driver_service.h>

#include "kos_seam.h"

#include <gtest/gtest.h>

#include <string.h>

static_assert(KICKOS_KERNEL_CORES > 1, "this gate is the multi-core build of the bring-up");

namespace drv = kickos::driver;

namespace
{
    constexpr uintptr_t K_BASE = 0x4000c000u;

    void t_irq(void*)
    {
    }

    void t_service(void*)
    {
    }

    // One line, a line holder waiting on the notification and a receiver that holds neither.
    constexpr drv::Descriptor k_bus = {
        .tag = "[drvsmp] ",
        .expected_base = K_BASE,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .line_count = 1,
        .thread_count = 2,
        .barrier_after = 2,
        .lines = {{KOS_IRQ_EDGE}},
        .threads = {{.entry = t_irq,
                     .name = "smpirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_WINDOW,
                     .window_grant = true,
                     .cap_count = 2,
                     .caps = {{drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0}}},
                    {.entry = t_service,
                     .name = "smpsvc",
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };
    static_assert(drv::valid(k_bus), "the multi-core gate descriptor is not a driver shape");

    class DrvCores : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            kos_seam_reset();
        }
    };

    TEST_F(DrvCores, the_lines_are_claimed_and_every_thread_runs_on_the_declared_core)
    {
        struct kos_driver_instance in = {};
        in.endpoint = 7u;
        in.watch = 8u;
        in.core_mask = 1u << 3;
        in.ceiling = 9u;
        in.line_count = 1;
        in.lines[0] = {16u, 0u};
        in.name = "drvsmp";
        in.mmio_base = K_BASE;
        in.mmio_window = 0x40u;
        in.priority = 8u;
        EXPECT_EQ(drv::bring_up(k_bus, &in), 0);
        EXPECT_STREQ(kos_seam_trace(), "task90 grant90/9/8 watch90/8/7 pin8 claim10 pin0 note11 badge0 bind close12"
                                       " spawn50 core8 spawn51 core8 close10 close11");
    }

    struct kos_driver_instance instance_on_core_3()
    {
        struct kos_driver_instance in = {};
        in.endpoint = 7u;
        in.watch = 8u;
        in.core_mask = 1u << 3;
        in.ceiling = 9u;
        in.line_count = 1;
        in.lines[0] = {16u, 0u};
        in.name = "drvsmp";
        in.mmio_base = K_BASE;
        in.mmio_window = 0x40u;
        in.priority = 8u;
        return in;
    }

    TEST_F(DrvCores, a_refused_claim_unpins_before_the_failure_is_reported)
    {
        g_seam.irq_claim_fail_at = 1;
        struct kos_driver_instance in = instance_on_core_3();
        EXPECT_EQ(drv::bring_up(k_bus, &in), -1);
        EXPECT_STREQ(kos_seam_trace(), "task90 grant90/9/8 watch90/8/7 pin8 claim! pin0 print print");
    }

    TEST_F(DrvCores, a_bring_up_that_cannot_pin_to_the_declared_core_claims_nothing)
    {
        g_seam.pin_fails = true;
        struct kos_driver_instance in = instance_on_core_3();
        EXPECT_EQ(drv::bring_up(k_bus, &in), -1);
        EXPECT_STREQ(kos_seam_trace(), "task90 grant90/9/8 watch90/8/7 pin! print print");
        EXPECT_NE(strstr(kos_seam_msg(), "could not pin to the declared core"), nullptr);
    }
}
