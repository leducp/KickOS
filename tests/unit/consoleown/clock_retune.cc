// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/time/clock_select.cc's console coherence over the real console translation units: the
// ownership it refuses on, and what it drains and re-primes around the retune.

#include <gtest/gtest.h>

#include <string>

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/time.h>

#include "console_seam.h"
#include "isolated_arm.h"

namespace
{
    constexpr uint32_t kRing = 128u;

    uint64_t g_hz = 48000000u;
    uint32_t g_retunes = 0;
    uint32_t g_clock_sets = 0;
}

extern "C"
{
    uint64_t arch_cpu_clock_hz(void)
    {
        return g_hz;
    }

    uint64_t arch_cpu_clock_set(uint32_t)
    {
        g_clock_sets = g_clock_sets + 1;
        g_hz = g_hz * 2u;
        return g_hz;
    }

    // As a chip whose baud rewrite clears its TX interrupt enable.
    void arch_console_retune(void)
    {
        g_retunes = g_retunes + 1;
        consoleseam::set_tx_irq_enabled(false);
    }
}

namespace kickos
{
    void ktime_disarm(Held)
    {
    }

    void ktime_rearm(Thread const*, Held)
    {
    }
}

// A publish landing as the retune is about to mask: the driver's baud is left alone.
TEST(ConsoleClockRetune, APublishBeforeTheRetuneMasksLeavesTheDriversBaud)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::run_at_mask([]() {
            consoleseam::publish_begin();
            consoleseam::publish_end();
        });
        uint64_t const before = g_hz;
        EXPECT_EQ(kickos::cpu_clock_set(1), before);
        EXPECT_EQ(console_owner_is_kernel(), 0) << "fixture: the publish never landed";
        EXPECT_EQ(g_clock_sets, 0u) << "the clock moved under a published console";
        EXPECT_EQ(g_retunes, 0u) << "the retune rewrote the driver's baud";
    });
}

// Bytes queued in the ring stay there across the retune, which only drains the device, and the
// drain is turned back on after it: they go out, in order, once the mask opens.
TEST(ConsoleClockRetune, TheRetuneKeepsTheRingAndReprimesItsDrain)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_slot_free(0);
        std::string const queued = "queued across the retune\n";
        kickos::kputs(queued.c_str());
        ASSERT_EQ(consoleseam::wire(), "") << "fixture: a byte left before the retune";
        consoleseam::set_slot_free(1);
        ASSERT_EQ(consoleseam::max_masked_pushes(), 0u);
        EXPECT_NE(kickos::cpu_clock_set(1), 0u);
        EXPECT_EQ(g_retunes, 1u);
        // One: a device whose TX interrupt is transition-triggered raises none while idle.
        EXPECT_EQ(consoleseam::max_masked_pushes(), 1u)
            << "the retune sent the ring's bytes with the mask held, or primed nothing";
        EXPECT_EQ(consoleseam::wire(), queued) << "the retune left the ring's drain off";
    });
}
