// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K64F DSPI engine (system/driver/mk64f/k64dspi/spi_dspi.cc) over host buffers for DSPI0's
// window and the GPIO ports. The RX fill level is this file's dspi_rx_count, so a case decides
// on which read a received frame arrives: never, and the transfer must end -KOS_EIO with the
// module halted, its FIFOs flushed and the chip select released, and must not spin; or only past
// KICKOS_POLL_SPIN_MAX reads at a slow divider pair, and the transfer must still complete. Close
// must re-protect the block after its last register store, and a refused open leaves no base.
//
// What this cannot witness: the bound's margin over a real frame on silicon (frdmk64f's bench).

#include <kickos/driver/spi.h>
#include <kickos/poll_bound.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <dspi_class.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace mmap_k64 = kickos::mk64f::mmap;

namespace
{
    constexpr uintptr_t MCR = 0x00u;
    constexpr uintptr_t PSOR = 0x04u;
    constexpr uintptr_t PCOR = 0x08u;
    constexpr uint32_t CS_BIT = 1u << 4; // PTC4

    constexpr uint32_t MCR_MSTR = 1u << 31;
    constexpr uint32_t MCR_CLR_TXF = 1u << 11;
    constexpr uint32_t MCR_CLR_RXF = 1u << 10;
    constexpr uint32_t MCR_HALT = 1u << 0;
    constexpr uint32_t MCR_HALTED = MCR_MSTR | MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT;

    // 60 MHz / (2 * 32768), the fastest pair at or below 1 kHz: 524288 bus clocks a frame.
    constexpr uint32_t SLOW_HZ = 1000u;
    constexpr int32_t SLOW_RATE = 915;

    uint32_t g_enables = 0;
    int g_enable_rc = 0;
    uint32_t g_disables = 0;
    uintptr_t g_disabled_base = 0;
    int g_disable_rc = 0;
    uint32_t g_mcr_at_disable = 0;
    uint32_t g_psor_at_disable = 0;

    // The read on which one received frame shows; 0 never.
    uint32_t g_rx_reads = 0;
    uint32_t g_rx_on_read = 0;

    uintptr_t gpioc()
    {
        return mmap_k64::GPIOA_BASE + 2u * mmap_k64::GPIO_STRIDE;
    }

    volatile uint32_t& reg(uintptr_t addr)
    {
        return *reinterpret_cast<volatile uint32_t*>(addr);
    }

    // The pre-fix pump never returns here, so the arm ends the process rather than hang ctest.
    int32_t transfer_or_die(struct kos_spi_device* d, unsigned char* buf, uint32_t len)
    {
        struct kos_bus_seg seg = {static_cast<uint16_t>(len), 0u, 0u};
        std::packaged_task<int32_t()> job([d, &seg, buf, len]() {
            return kos_spi_transfer(d, &seg, 1u, buf, len);
        });
        std::future<int32_t> done = job.get_future();
        std::thread runner(std::move(job));
        if (done.wait_for(std::chrono::seconds(60)) != std::future_status::ready)
        {
            fprintf(stderr, "kos_spi_transfer never returned\n");
            fflush(stderr);
            _Exit(1);
        }
        runner.join();
        return done.get();
    }

    struct kos_spi_bus_config bus_config()
    {
        struct kos_spi_bus_config cfg = {};
        cfg.base = mmap_k64::DSPI0_BASE;
        cfg.ep = KOS_CAP_NONE;
        cfg.irq = KOS_CAP_NONE;
        cfg.notify = KOS_CAP_NONE;
        return cfg;
    }

    int32_t open_device(struct kos_spi_device* d, struct kos_spi_bus* b, uint32_t hz)
    {
        struct kos_spi_device_config dcfg = {};
        dcfg.hz = hz;
        dcfg.word_bits = 8u;
        dcfg.cs_policy = KOS_BUS_CS_GPIO;
        dcfg.cs_index = 0u;
        return kos_spi_device_open(d, b, &dcfg);
    }

    class DspiStall : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            memset(mmap_k64::g_dspi0_window, 0, sizeof(mmap_k64::g_dspi0_window));
            memset(mmap_k64::g_gpio_ports, 0, sizeof(mmap_k64::g_gpio_ports));
            g_enables = 0;
            g_enable_rc = 0;
            g_disables = 0;
            g_disabled_base = 0;
            g_disable_rc = 0;
            g_mcr_at_disable = 0;
            g_psor_at_disable = 0;
            g_rx_reads = 0;
            g_rx_on_read = 0;

            struct kos_spi_bus_config const cfg = bus_config();
            ASSERT_EQ(kos_spi_bus_open(&bus_, &cfg), 0);
            ASSERT_GT(open_device(&dev_, &bus_, 1000000u), 0);
        }

        struct kos_spi_bus bus_;
        struct kos_spi_device dev_;
    };
}

uint32_t kickos::mk64f::driver::dspi_rx_count(uintptr_t)
{
    g_rx_reads++;
    if (g_rx_on_read != 0u and g_rx_reads == g_rx_on_read)
    {
        return 1u;
    }
    return 0u;
}

extern "C"
{
    int kos_periph_enable(uintptr_t)
    {
        g_enables++;
        return g_enable_rc;
    }

    int kos_periph_disable(uintptr_t base)
    {
        g_disables++;
        g_disabled_base = base;
        g_mcr_at_disable = reg(base + MCR);
        g_psor_at_disable = reg(gpioc() + PSOR);
        return g_disable_rc;
    }

    uint32_t kos_periph_clock_hz(uintptr_t)
    {
        return 60000000u;
    }

    int kos_pinmux_set(uint32_t, uint32_t, uint32_t)
    {
        return 0;
    }
}

TEST_F(DspiStall, a_frame_that_never_completes_ends_the_transfer_with_eio)
{
    reg(gpioc() + PSOR) = 0u;
    unsigned char buf[7] = {1u, 2u, 3u, 4u, 5u, 6u, 7u};

    EXPECT_EQ(transfer_or_die(&dev_, buf, sizeof(buf)), -KOS_EIO);

    EXPECT_EQ(reg(mmap_k64::DSPI0_BASE + MCR), MCR_HALTED)
        << "the module is left halted with both FIFOs flushed";
    EXPECT_EQ(reg(gpioc() + PCOR), CS_BIT) << "the chip select was asserted";
    EXPECT_EQ(reg(gpioc() + PSOR), CS_BIT) << "and is released before the error returns";
}

// One frame at this pair is more bus clocks than KICKOS_POLL_SPIN_MAX, and a poll costs at least
// one, so a frame arriving past that many idle polls is a healthy frame.
TEST_F(DspiStall, a_slow_frame_past_the_poll_bound_still_completes)
{
    struct kos_spi_device slow;
    ASSERT_EQ(open_device(&slow, &bus_, SLOW_HZ), SLOW_RATE);
    g_rx_on_read = KICKOS_POLL_SPIN_MAX + 1000u;
    unsigned char buf[1] = {0x5Au};

    EXPECT_EQ(transfer_or_die(&slow, buf, sizeof(buf)), 1);
    EXPECT_EQ(g_rx_reads, g_rx_on_read);
}

// The CTAR delays at their longest, 7 x 65536 bus clocks each, outweigh KICKOS_POLL_SPIN_MAX
// together at the fastest rate, so the bound must count them. The engine programs none of its
// own, so the device's folded CTAR word, prog[0], is set here.
TEST_F(DspiStall, a_frame_at_the_longest_delays_past_the_poll_bound_still_completes)
{
    constexpr uint32_t ALL_DELAYS_LONGEST = (3u << 22) | (3u << 20) | (3u << 18) | (0xFu << 12)
                                            | (0xFu << 8) | (0xFu << 4);
    dev_.prog[0] |= ALL_DELAYS_LONGEST;
    g_rx_on_read = KICKOS_POLL_SPIN_MAX + 100000u;
    unsigned char buf[1] = {0xA5u};

    EXPECT_EQ(transfer_or_die(&dev_, buf, sizeof(buf)), 1);
    EXPECT_EQ(g_rx_reads, g_rx_on_read);
}

TEST_F(DspiStall, close_disables_the_block_after_its_last_access)
{
    reg(gpioc() + PSOR) = 0u;
    EXPECT_EQ(kos_spi_bus_close(&bus_), 0);
    EXPECT_EQ(g_disables, 1u);
    EXPECT_EQ(g_disabled_base, mmap_k64::DSPI0_BASE);
    EXPECT_EQ(g_mcr_at_disable, MCR_HALTED) << "the module was halted before the slot shut";
    EXPECT_EQ(g_psor_at_disable, CS_BIT) << "and the chip select released";

    EXPECT_EQ(kos_spi_bus_close(&bus_), 0) << "a second close is a no-op";
    EXPECT_EQ(g_disables, 1u);
}

TEST_F(DspiStall, close_answers_a_refused_disable)
{
    g_disable_rc = -KOS_EPERM;
    EXPECT_EQ(kos_spi_bus_close(&bus_), -KOS_EPERM);
    EXPECT_EQ(bus_.base, 0u) << "the bus is closed all the same";
}

TEST_F(DspiStall, a_refused_enable_leaves_no_open_bus)
{
    struct kos_spi_bus refused;
    refused.base = 0x1234u;
    g_enable_rc = -KOS_EPERM;
    reg(gpioc() + PSOR) = 0u;
    struct kos_spi_bus_config const cfg = bus_config();

    EXPECT_EQ(kos_spi_bus_open(&refused, &cfg), -KOS_EPERM);
    EXPECT_EQ(refused.base, 0u) << "nothing may transfer through a bus whose enable was refused";
    EXPECT_EQ(reg(gpioc() + PSOR), CS_BIT) << "the chip selects idle all the same";
    EXPECT_EQ(kos_spi_bus_close(&refused), 0);
    EXPECT_EQ(g_disables, 0u) << "and closing it disables nothing";
}
