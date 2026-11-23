// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RX72M RIICa engine's device open (system/driver/rx72m/i2c_riic.cc) over a host buffer for
// one channel's byte window. The bus is bound by hand: the bring-up's line monitors read the
// pins, which a buffer cannot model.

#include <kickos/driver/i2c.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <stdint.h>
#include <string.h>

namespace
{
    constexpr uint32_t PCLKB_HZ = 60000000u;

    alignas(32) uint8_t g_window[0x20];

    int32_t open_device(struct kos_i2c_device* d, struct kos_i2c_bus* b, uint16_t addr, uint32_t hz)
    {
        struct kos_i2c_device_config cfg = {};
        cfg.hz = hz;
        cfg.addr = addr;
        return kos_i2c_device_open(d, b, &cfg);
    }
}

extern "C"
{
    int kos_periph_enable(uintptr_t)
    {
        return 0;
    }

    uint32_t kos_periph_clock_hz(uintptr_t)
    {
        return PCLKB_HZ;
    }

    int64_t kickos_monotonic_now(void)
    {
        return 0;
    }
}

TEST(RiicOpen, a_device_opened_at_rate_zero_is_refused_after_another_set_the_rate)
{
    memset(g_window, 0, sizeof(g_window));
    struct kos_i2c_bus bus = {};
    bus.base = reinterpret_cast<uintptr_t>(g_window);
    bus.ep = KOS_CAP_NONE;
    bus.irq = KOS_CAP_NONE;

    struct kos_i2c_device first;
    ASSERT_GT(open_device(&first, &bus, 0x50u, 100000u), 0);

    struct kos_i2c_device second;
    EXPECT_EQ(open_device(&second, &bus, 0x51u, 0u), -KOS_EINVAL) << "the live divider is the other device's";
}
