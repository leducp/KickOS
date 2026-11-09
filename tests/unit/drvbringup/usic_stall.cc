// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The XMC4800 USIC SSC engine (system/driver/xmc4800/xmcssc/spi_usic.cc) over a host buffer for
// its window. The fake kos_notify_wait plays the line: a case decides how long a word takes to
// raise its receive event, or that the line goes silent from some word on. A silent line must end
// the transfer -KOS_EIO with the channel stopped and its chip select idle; a word at the slowest
// clock setting and the longest delays must still complete.
//
// What this cannot witness: the bound's margin over a real word on silicon (xmc4800-relax).

#include <kickos/driver/spi.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <regs/usic.h>

#include <gtest/gtest.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ru = kickos::xmc::reg::usic;

namespace
{
    constexpr uint32_t PERIPH_HZ = 144000000u;
    constexpr uint32_t CCR_RUNNING = ru::CCR_MODE_SSC | ru::CCR_RIEN | ru::CCR_AIEN;
    constexpr uint32_t RX_FLAGS = ru::PSR_RIF | ru::PSR_AIF;
    constexpr uint32_t NEVER = UINT32_MAX;

    // FDR.STEP = 1 and BRG.PDIV = 1023: 144 MHz / 1024 / (2 * 1024), 68.66 Hz.
    constexpr uint32_t SLOW_FDR = ru::FDR_DM_FRACTIONAL | 1u;
    constexpr uint32_t SLOW_BRG = 1023u << ru::BRG_PDIV_SHIFT;
    constexpr int32_t SLOW_RATE = 68;

    // Eight bits plus four slave-select delays of 4 x 32 shift clocks each, at the exact rate.
    constexpr uint64_t SLOW_WORD_US = (520ull * 1000000ull * 2097152ull + PERIPH_HZ - 1u) / PERIPH_HZ;

    alignas(512) uint32_t g_window[0x200u / 4u];

    uint32_t g_word_us = 0;
    uint32_t g_silent_from = NEVER;
    bool g_lands_late = false;

    uint32_t g_words = 0;
    uint32_t g_polls = 0;
    uint32_t g_ccr_at_poll = 0;
    uint32_t g_fmr_at_poll = 0;
    uint32_t g_pscr_at_poll = 0;

    uint32_t& word(uintptr_t offset)
    {
        return g_window[offset / 4u];
    }

    class UsicStall : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            memset(g_window, 0, sizeof(g_window));
            g_word_us = 0;
            g_silent_from = NEVER;
            g_lands_late = false;
            g_words = 0;
            g_polls = 0;
            g_ccr_at_poll = 0;
            g_fmr_at_poll = 0;
            g_pscr_at_poll = 0;

            struct kos_spi_bus_config cfg = {};
            cfg.base = reinterpret_cast<uintptr_t>(g_window);
            cfg.ep = KOS_CAP_NONE;
            cfg.irq = 3;
            cfg.notify = 4;
            cfg.notify_bit = 0u;
            cfg.irq_index = 1u;
            ASSERT_EQ(kos_spi_bus_open(&bus_, &cfg), 0);
            ASSERT_GT(open_device(&dev_), 0);
        }

        int32_t open_device(struct kos_spi_device* d)
        {
            struct kos_spi_device_config dcfg = {};
            dcfg.word_bits = 8u;
            dcfg.cs_policy = KOS_BUS_CS_HW;
            return kos_spi_device_open(d, &bus_, &dcfg);
        }

        int32_t transfer(struct kos_spi_device* d, unsigned char* buf, uint32_t len)
        {
            struct kos_bus_seg seg = {static_cast<uint16_t>(len), 0u, 0u};
            return kos_spi_transfer(d, &seg, 1u, buf, len);
        }

        struct kos_spi_bus bus_;
        struct kos_spi_device dev_;
    };
}

extern "C"
{
    int kos_notify_bind(kos_cap_t)
    {
        return 0;
    }

    int kos_notify_wait(kos_cap_t, uint32_t, uint32_t timeout_us, uint32_t*)
    {
        // PSCR is write-1-to-clear on PSR, which a host buffer only stores.
        uint32_t const pscr = word(ru::off::PSCR);
        word(ru::off::PSR) &= ~(pscr & RX_FLAGS);
        word(ru::off::PSCR) = 0u;

        if (timeout_us == 0u)
        {
            g_polls++;
            g_ccr_at_poll = word(ru::off::CCR);
            g_fmr_at_poll = word(ru::off::FMR);
            g_pscr_at_poll = pscr;
            return -KOS_ETIMEDOUT;
        }

        uint32_t const w = g_words++;
        if (w >= g_silent_from)
        {
            if (timeout_us == KOS_TIMEOUT_NONE)
            {
                fprintf(stderr, "a wait with no bound on a silent line never returns\n");
                fflush(stderr);
                _Exit(1);
            }
            return -KOS_ETIMEDOUT;
        }
        if (g_word_us > timeout_us)
        {
            return -KOS_ETIMEDOUT;
        }
        word(ru::off::RBUF) = word(ru::off::TBUF0); // the engine's internal loop-back
        word(ru::off::PSR) |= ru::PSR_AIF;
        if (g_lands_late)
        {
            return -KOS_ETIMEDOUT;
        }
        return 0;
    }

    int kos_irq_ack(kos_cap_t)
    {
        return 0;
    }

    uint32_t kos_periph_clock_hz(uintptr_t)
    {
        return PERIPH_HZ;
    }

    int kos_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value)
    {
        memcpy(reinterpret_cast<void*>(base + offset), &value, sizeof(value));
        return 0;
    }
}

TEST_F(UsicStall, a_silent_line_ends_the_transfer_with_eio_and_the_channel_stopped)
{
    g_silent_from = 1u;
    unsigned char buf[3] = {0x11u, 0x22u, 0x33u};

    EXPECT_EQ(transfer(&dev_, buf, sizeof(buf)), -KOS_EIO);

    EXPECT_EQ(g_words, 2u) << "the transfer gave up on the first silent word";
    EXPECT_EQ(word(ru::off::CCR), 0u) << "the shift clock and the receive sources are off";
    ASSERT_EQ(g_polls, 1u) << "a bit banked after the expiry was drained";
    EXPECT_EQ(g_ccr_at_poll, 0u) << "after the stop";
    EXPECT_EQ(g_fmr_at_poll, ru::FMR_MTDV_CLEAR) << "a word not yet shifted was dropped";
    EXPECT_EQ(g_pscr_at_poll & ru::PSCR_MSLS, ru::PSCR_MSLS) << "the chip select is idle";
}

TEST_F(UsicStall, the_transfer_after_a_stall_restarts_the_channel)
{
    g_silent_from = 0u;
    unsigned char lost[1] = {0x5Au};
    ASSERT_EQ(transfer(&dev_, lost, sizeof(lost)), -KOS_EIO);
    ASSERT_EQ(word(ru::off::CCR), 0u);

    g_silent_from = NEVER;
    unsigned char buf[2] = {0xC3u, 0x3Cu};
    EXPECT_EQ(transfer(&dev_, buf, sizeof(buf)), 2);
    EXPECT_EQ(word(ru::off::CCR), CCR_RUNNING);
    EXPECT_EQ(buf[0], 0xC3u);
    EXPECT_EQ(buf[1], 0x3Cu);
}

TEST_F(UsicStall, a_word_whose_event_races_the_expiry_is_still_progress)
{
    g_lands_late = true;
    unsigned char buf[2] = {0xA1u, 0xB2u};

    EXPECT_EQ(transfer(&dev_, buf, sizeof(buf)), 2);
    EXPECT_EQ(word(ru::off::CCR), CCR_RUNNING);
    EXPECT_EQ(buf[0], 0xA1u);
    EXPECT_EQ(buf[1], 0xB2u);
}

TEST_F(UsicStall, a_word_at_the_slowest_clock_and_the_longest_delays_still_completes)
{
    word(ru::off::FDR) = SLOW_FDR;
    word(ru::off::BRG) = SLOW_BRG;
    struct kos_spi_device slow;
    ASSERT_EQ(open_device(&slow), SLOW_RATE);
    g_word_us = static_cast<uint32_t>(SLOW_WORD_US);
    unsigned char buf[1] = {0x96u};

    EXPECT_EQ(transfer(&slow, buf, sizeof(buf)), 1);
    EXPECT_EQ(buf[0], 0x96u);
}
