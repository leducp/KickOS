// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The lookups against qemu-x86_64.yaml with the sensor also granted the HPET: it holds the
// HPET window at place 0, the CMOS port range at place 1 and the history at place 2.

#include "lookup_seam.h"

#include <kickos/sys.h>

#include <gtest/gtest.h>

namespace
{
    constexpr uint32_t PAGE = 0x1000u;
    constexpr uintptr_t HPET_MAPPED = 0x7000000u;
    constexpr uintptr_t CMOS_FIRST_PORT = 0x70u;
    constexpr uint32_t CMOS_PORTS = 2u;

    alignas(4096) unsigned char g_history[PAGE];

    void expect_no_window(kos_window_t w)
    {
        EXPECT_EQ(kos_window_addr(w), nullptr);
        EXPECT_EQ(kos_window_size(w), 0u);
    }

    class Lookup : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            seam::reset();
            seam::windows = {seam::window(HPET_MAPPED, PAGE, KOS_WINDOW_DEVICE, 0),
                             seam::window(CMOS_FIRST_PORT, CMOS_PORTS, KOS_WINDOW_PORTS, 0),
                             seam::window(reinterpret_cast<uintptr_t>(g_history), PAGE, KOS_WINDOW_MEMORY, 0)};
        }
    };

    TEST_F(Lookup, a_port_range_answers_its_first_port_and_count)
    {
        kos_window_t const cmos = kos_grant_ports(seam::task("sensor"), "/dev/cmos_rtc");
        EXPECT_EQ(reinterpret_cast<uintptr_t>(kos_window_addr(cmos)), CMOS_FIRST_PORT);
        EXPECT_EQ(kos_window_size(cmos), CMOS_PORTS);
        EXPECT_EQ(seam::asked, std::vector<uint32_t>{1u});
    }

    TEST_F(Lookup, each_window_kind_is_asked_at_its_own_place)
    {
        kos_self_t const* const sensor = seam::task("sensor");
        EXPECT_EQ(reinterpret_cast<uintptr_t>(kos_window_addr(kos_grant_mmio(sensor, "/dev/hpet"))), HPET_MAPPED);
        EXPECT_EQ(kos_window_addr(kos_grant_mem(sensor, "/shm/history")), g_history);
        EXPECT_EQ(seam::asked, (std::vector<uint32_t>{0u, 2u}));
    }

    TEST_F(Lookup, a_port_range_and_a_device_window_are_not_each_other)
    {
        kos_self_t const* const sensor = seam::task("sensor");
        expect_no_window(kos_grant_mmio(sensor, "/dev/cmos_rtc"));
        expect_no_window(kos_grant_ports(sensor, "/dev/hpet"));
        expect_no_window(kos_grant_mem(sensor, "/dev/cmos_rtc"));
    }

    TEST_F(Lookup, a_port_window_of_another_kind_or_count_answers_invalid)
    {
        seam::windows[1].kind = KOS_WINDOW_DEVICE;
        expect_no_window(kos_grant_ports(seam::task("sensor"), "/dev/cmos_rtc"));
        seam::windows[1].kind = KOS_WINDOW_PORTS;
        seam::windows[1].size = CMOS_PORTS + 1u;
        expect_no_window(kos_grant_ports(seam::task("sensor"), "/dev/cmos_rtc"));
    }
}
