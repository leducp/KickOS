// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP USB backend's suspend and resume report (system/driver/rp2xxx/rpusb/rp_usb_dev.h) over a
// model of the controller's suspend half: a bus edge that lands as the backend clears its
// interrupt is not lost, and the line goes quiet only once the last report matches the bus.

#include "rp_usb_dev.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace reg = kickos::rpusb::reg;
    namespace usb = kickos::usb;

    // As both datasheets document it: SIE_STATUS.SUSPENDED reads the bus state, INTR.DEV_SUSPEND
    // is a latch of its own, raised on every change of that state and cleared by writing
    // SUSPENDED, and SIE_STATUS.RESUME is a write-1-to-clear latch behind DEV_RESUME_FROM_HOST.
    struct Bus
    {
        bool suspended = false;
        bool dev_suspend = false;
        bool resume = false;
        // Runs once, as the next write to SIE_STATUS is about to land.
        void (*before_status_write)() = nullptr;

        void host_suspend()
        {
            suspended = true;
            dev_suspend = true;
        }

        void host_resume()
        {
            suspended = false;
            dev_suspend = true;
            resume = true;
        }

        uint32_t ints() const
        {
            uint32_t v = 0u;
            if (dev_suspend)
            {
                v |= reg::INT_DEV_SUSPEND;
            }
            if (resume)
            {
                v |= reg::INT_DEV_RESUME_FROM_HOST;
            }
            return v;
        }

        uint32_t sie_status() const
        {
            uint32_t v = 0u;
            if (suspended)
            {
                v |= reg::SIE_STATUS_SUSPENDED;
            }
            if (resume)
            {
                v |= reg::SIE_STATUS_RESUME;
            }
            return v;
        }
    };

    Bus g_bus;

    struct Line
    {
        usb::Shared sh;
        kickos::rpusb::RpUsb dev;
        bool told_suspended = false;

        Line()
        {
            g_bus = Bus{};
            dev.sh = &sh;
            dev.dpram = reg::DPRAM_BASE;
            dev.regs = reg::REGS_BASE;
        }

        // The level line: the IRQ thread takes passes while INTS holds a bit.
        void serve()
        {
            for (int pass = 0; pass < 4 and g_bus.ints() != 0u; pass++)
            {
                uint32_t const ev = dev.take_events();
                bool const suspend = (ev & usb::KOS_USB_EV_SUSPEND) != 0u;
                bool const resume = (ev & usb::KOS_USB_EV_RESUME) != 0u;
                EXPECT_FALSE(suspend and resume);
                if (suspend)
                {
                    told_suspended = true;
                }
                if (resume)
                {
                    told_suspended = false;
                }
            }
            EXPECT_EQ(g_bus.ints(), 0u);
        }
    };
}

uint32_t mmio_read(uintptr_t addr, uint32_t width)
{
    EXPECT_EQ(width, 4u);
    if (addr == reg::REGS_BASE + reg::INTS)
    {
        return g_bus.ints();
    }
    if (addr == reg::REGS_BASE + reg::SIE_STATUS)
    {
        return g_bus.sie_status();
    }
    ADD_FAILURE() << "unmodelled read at 0x" << std::hex << addr;
    return 0u;
}

void mmio_write(uintptr_t addr, uint32_t width, uint32_t value)
{
    EXPECT_EQ(width, 4u);
    if (addr == reg::REGS_BASE + reg::SIE_STATUS)
    {
        void (*const hook)() = g_bus.before_status_write;
        g_bus.before_status_write = nullptr;
        if (hook != nullptr)
        {
            hook();
        }
        if ((value & reg::SIE_STATUS_SUSPENDED) != 0u)
        {
            g_bus.dev_suspend = false;
        }
        if ((value & reg::SIE_STATUS_RESUME) != 0u)
        {
            g_bus.resume = false;
        }
        return;
    }
    ADD_FAILURE() << "unmodelled write at 0x" << std::hex << addr;
}

TEST(RpUsbEvents, a_suspend_and_a_resume_each_report_the_bus_state)
{
    Line l;
    g_bus.host_suspend();
    l.serve();
    EXPECT_TRUE(l.told_suspended);
    g_bus.host_resume();
    l.serve();
    EXPECT_FALSE(l.told_suspended);
}

TEST(RpUsbEvents, a_resume_landing_as_the_backend_clears_is_not_lost)
{
    Line l;
    g_bus.host_suspend();
    g_bus.before_status_write = [] { g_bus.host_resume(); };
    l.serve();
    EXPECT_FALSE(l.told_suspended);
}

TEST(RpUsbEvents, a_suspend_landing_as_the_backend_clears_is_not_lost)
{
    Line l;
    g_bus.host_suspend();
    l.serve();
    g_bus.host_resume();
    g_bus.before_status_write = [] { g_bus.host_suspend(); };
    l.serve();
    EXPECT_TRUE(l.told_suspended);
}
