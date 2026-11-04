// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kickos::driver::bring_up built for more than one kernel core, over the recording seam: each
// thread it spawns is placed on the declared core, and the entry thread, which the kernel's
// create placed there and claimed the lines for, pins nothing.

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
        .block_init = nullptr,
        .launch = nullptr
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

    TEST_F(DrvCores, every_thread_runs_on_the_declared_core_and_nothing_pins)
    {
        struct kos_driver_instance in = {};
        in.core_mask = 1u << 3;
        in.ceiling = 9u;
        in.line_count = 1;
        in.lines[0] = {16u, 0u};
        in.name = "drvsmp";
        in.mmio_base = K_BASE;
        in.mmio_window = 0x40u;
        in.priority = 8u;
        try
        {
            (void)drv::bring_up(k_bus, &in);
            ADD_FAILURE() << "bring_up returned past its spawns";
        }
        catch (kos_seam_exit const& e)
        {
            EXPECT_EQ(e.code, 0);
        }
        EXPECT_STREQ(kos_seam_trace(), "spawn50/p8 core8 g3/1 narrow1/1 narrow2/1 close3 prio9 exit0");
    }
}
