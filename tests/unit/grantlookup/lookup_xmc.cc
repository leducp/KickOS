// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The lookups against xmc4800-relax.yaml as written: two packaged drivers, whose grants record
// neither a slot nor a window place, a sensor that uses /svc/spi0 at slot 0 before serving
// /svc/sensor at slot 1, and health watching spi0 then sensor through a 32-byte status block of
// its own.

#include "lookup_seam.h"

#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/init_status.h>

#include <gtest/gtest.h>

extern "C"
{

int xmc_spi0_start(struct kos_driver_instance* instance)
{
    (void)instance;
    return 0;
}

int xmcuartirq_console_start(struct kos_driver_instance* instance)
{
    (void)instance;
    return 0;
}

}

namespace
{
    constexpr kos_cap_t CAP0 = KOS_SPAWN_DELEGATED_CAP0;
    // health's own two records, rounded as a region is.
    constexpr uint32_t STATUS_SIZE = 0x20u;
    constexpr uint32_t PAGE = 0x1000u;

    class Lookup : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            seam::reset();
        }
    };

    TEST_F(Lookup, a_used_endpoint_ahead_of_the_served_one_takes_the_first_slot)
    {
        EXPECT_EQ(seam::grant_endpoint(seam::task("sensor"), "/svc/spi0"), CAP0 + 0u);
        EXPECT_EQ(seam::grant_endpoint(seam::task("sensor"), "/svc/sensor"), CAP0 + 1u);
    }

    // The window a grant with no place would alias, KOS_TABLE_NONE taken as an index, answers
    // here, so only a lookup that asks nothing refuses it. The endpoint a driver serves is at its
    // entry thread's place of it.
    TEST_F(Lookup, a_packaged_drivers_grants_answer_invalid_but_its_served_endpoint)
    {
        seam::windows.assign(KOS_TABLE_NONE + 1u, seam::window(0, 0, KOS_WINDOW_MEMORY, 0));
        seam::windows[KOS_TABLE_NONE] = seam::window(0x40030000u, 0x200u, KOS_WINDOW_DEVICE, 0);
        EXPECT_EQ(seam::grant_endpoint(seam::task("console"), "/svc/console"), CAP0 + 2u)
            << "after the notification and the line thread 0 holds";
        EXPECT_EQ(seam::grant_endpoint(seam::task("spi0"), "/svc/spi0"), CAP0 + 0u)
            << "where thread 0 holds it";
        for (char const* name : {"console", "spi0"})
        {
            kos_self_t const* const driver = seam::task(name);
            EXPECT_EQ(seam::grant_endpoint(driver, "/svc/sensor"), KOS_CAP_NONE);
            EXPECT_EQ(seam::grant_notify(driver, "/init/events"), KOS_CAP_NONE);
            kos_line_t const irq = seam::grant_irq(driver, "irq");
            EXPECT_EQ(irq.cap, KOS_CAP_NONE);
            EXPECT_EQ(irq.index, KOS_TABLE_NONE);
            kos_window_t const regs = seam::grant_mmio(driver, "regs");
            EXPECT_EQ(kos_window_addr(regs), nullptr);
            EXPECT_EQ(kos_window_size(regs), 0u);
        }
        EXPECT_TRUE(seam::asked.empty());
    }

    TEST_F(Lookup, the_status_read_follows_the_watch_order)
    {
        seam::StatusBlock block(PAGE);
        seam::windows = {seam::window(block.base(), STATUS_SIZE, KOS_WINDOW_MEMORY, KOS_WINDOW_RO)};
        kickos::init::status_write(block.record(seam::watched_at("health", "spi0")),
                                   kickos::init::StatusFields{2, 1, kickos::init::STATUS_ALIVE});
        kickos::init::status_write(block.record(seam::watched_at("health", "sensor")),
                                   kickos::init::StatusFields{3, 0, 0});

        struct kos_task_status st;
        ASSERT_EQ(seam::task_status(seam::task("health"), 0, &st), 0);
        EXPECT_STREQ(st.name, "spi0");
        EXPECT_TRUE(st.alive);
        EXPECT_EQ(st.deaths, 2u);
        EXPECT_EQ(st.restarts_left, 1u);
        ASSERT_EQ(seam::task_status(seam::task("health"), 1, &st), 0);
        EXPECT_STREQ(st.name, "sensor");
        EXPECT_FALSE(st.alive);
        EXPECT_FALSE(st.dependency_down);
        EXPECT_EQ(st.deaths, 3u);
        EXPECT_EQ(seam::task_status(seam::task("health"), 2, &st), -KOS_EINVAL);
    }
}
