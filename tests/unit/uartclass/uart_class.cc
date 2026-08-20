// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Contract gate for the raw UART class <kickos/driver/uart.h>: proves the five-call contract
// (open, read, write, flush, close) holds for a substituted backend, uart_mock.cc.

#include <kickos/driver/uart.h>

#include <kickos/sys/errno.h>

#include "uart_mock.h"

#include <stdint.h>

#include <gtest/gtest.h>

namespace
{
    // A channel opened on a fresh model, at the one frame the mock expresses.
    void configure(struct kos_uart_config* cfg, struct kos_uart_mock* m,
                   struct kos_uart_stats* stats)
    {
        cfg->base = reinterpret_cast<uintptr_t>(m);
        cfg->stats = stats;
        cfg->baud = 115200;
        cfg->data_bits = 8;
        cfg->parity = KOS_UART_PARITY_NONE;
        cfg->stop_bits = 1;
    }
}

// The kos_uart_cfg_check clauses, measured through the class entry point, so a backend that
// dropped the call fails here.
TEST(UartClass, open_refuses_a_malformed_config)
{
    struct kos_uart_mock m = {};
    m.rate = 115199;
    struct kos_uart_stats stats = {};
    struct kos_uart dev;

    struct kos_uart_config nobase = {};
    configure(&nobase, &m, &stats);
    nobase.base = 0u;
    EXPECT_EQ(kos_uart_open(&dev, &nobase), -KOS_EINVAL) << "a zero base is refused";

    struct kos_uart_config nostats = {};
    configure(&nostats, &m, &stats);
    nostats.stats = nullptr;
    EXPECT_EQ(kos_uart_open(&dev, &nostats), -KOS_EINVAL) << "a null stats block is refused";

    struct kos_uart_config dirty_rsv = {};
    configure(&dirty_rsv, &m, &stats);
    dirty_rsv.rsv = 1u;
    EXPECT_EQ(kos_uart_open(&dev, &dirty_rsv), -KOS_EINVAL) << "a non-zero rsv is refused";

    EXPECT_EQ(m.opened, 0u) << "no refused open bound the channel";
}

TEST(UartClass, open_refuses_an_unexpressible_frame)
{
    struct kos_uart_mock m = {};
    m.rate = 115199;
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    configure(&cfg, &m, &stats);

    // Refused BEFORE it binds: a consumer that ignored the return must not find a half-open
    // channel.
    struct kos_uart_config bad = cfg;
    bad.data_bits = 9;
    struct kos_uart dev;
    EXPECT_EQ(kos_uart_open(&dev, &bad), -KOS_ENOTSUP) << "a 9-bit frame is refused";
    EXPECT_EQ(m.opened, 0u) << "a refused open did not bind the channel";
}

TEST(UartClass, open_refuses_an_unknowable_rate)
{
    // No divisor to read back: a backend that cannot MEASURE the rate must refuse.
    struct kos_uart_mock m = {};
    m.rate = 0;
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    configure(&cfg, &m, &stats);

    struct kos_uart dev;
    EXPECT_EQ(kos_uart_open(&dev, &cfg), -KOS_ENOSYS) << "an unmeasurable rate is refused";
}

// kos_uart_cfg_check_fixed_rate, measured through the class entry point: the refusal value
// is THE CONTRACT'S, not a backend's.
TEST(UartClass, open_refuses_a_rate_request_it_cannot_program)
{
    struct kos_uart_mock m = {};
    m.rate = 115199;
    m.fixed_rate = 1;
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    configure(&cfg, &m, &stats);

    struct kos_uart dev;
    EXPECT_EQ(kos_uart_open(&dev, &cfg), -KOS_ENOTSUP) << "a rate request is refused";
    EXPECT_EQ(m.opened, 0u) << "a refused open did not bind the channel";

    // baud == 0 is the ONE request such a backend serves, and it still reports a measured
    // rate rather than the 0 it was handed.
    cfg.baud = 0;
    EXPECT_EQ(kos_uart_open(&dev, &cfg), 115199) << "adopting the running rate is served";
    EXPECT_EQ(m.opened, 1u) << "the served open bound the channel";
}

// A wedged transmit path, which is the one condition both bounded waits in the class share.
TEST(UartClass, a_transmit_path_that_will_not_drain_is_reported)
{
    struct kos_uart_mock m = {};
    m.rate = 115199;
    m.tx_stuck = 1;
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    configure(&cfg, &m, &stats);

    // OPEN REFUSES rather than reprogramming into a live shifter.
    struct kos_uart dev;
    EXPECT_EQ(kos_uart_open(&dev, &cfg), -KOS_EBUSY) << "open refuses an undrainable channel";
    EXPECT_EQ(m.opened, 0u) << "a refused open did not bind the channel";

    // FLUSH reports the same bound expiring, on a channel that did open.
    m.tx_stuck = 0;
    ASSERT_EQ(kos_uart_open(&dev, &cfg), 115199) << "open succeeded once the path drains";
    m.tx_stuck = 1;
    EXPECT_EQ(kos_uart_flush(&dev), -KOS_EBUSY) << "flush reports bytes still in flight";
    EXPECT_EQ(m.flushes, 1u) << "the refused flush still reached the device";
}

TEST(UartClass, open_reports_a_measured_rate)
{
    struct kos_uart_mock m = {};
    m.rate = 115199; // what the divider produces, never what a caller asks for
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    configure(&cfg, &m, &stats);

    struct kos_uart dev;
    int32_t const rate = kos_uart_open(&dev, &cfg);
    EXPECT_EQ(rate, 115199) << "open reports the modelled rate";
    EXPECT_NE(rate, static_cast<int32_t>(cfg.baud)) << "open does not echo the request";
    EXPECT_EQ(m.opened, 1u) << "open bound the channel once";
}

TEST(UartClass, read_on_an_idle_device_is_zero)
{
    struct kos_uart_mock m = {};
    m.rate = 115199;
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    configure(&cfg, &m, &stats);
    struct kos_uart dev;
    ASSERT_EQ(kos_uart_open(&dev, &cfg), 115199) << "open succeeded";

    // Nothing pending is 0, not an error.
    unsigned char in[8];
    EXPECT_EQ(kos_uart_read(&dev, in, sizeof(in)), 0u) << "an idle read returns 0";
    EXPECT_EQ(kos_counter_load(&stats.rx_bytes), 0u)
        << "an idle read counted nothing";
}

TEST(UartClass, write_is_short_and_owns_the_tx_arm)
{
    struct kos_uart_mock m = {};
    m.rate = 115199;
    m.tx_room = 4;
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    configure(&cfg, &m, &stats);
    struct kos_uart dev;
    ASSERT_EQ(kos_uart_open(&dev, &cfg), 115199) << "open succeeded";

    // A SHORT WRITE IS THE ORDINARY CASE: the count is what the device took, and the refusal
    // is what arms the TX source.
    unsigned char const six[6] = {'a', 'b', 'c', 'd', 'e', 'f'};
    EXPECT_EQ(kos_uart_write(&dev, six, 6), 4u) << "write returns what the device took";
    EXPECT_EQ(m.tx_len, 4u) << "the device holds the accepted bytes";
    EXPECT_EQ(m.tx_armed, 1u) << "a refused byte armed the TX source";

    // A call the device took WHOLE disarms it, zero-length included, or a consumer draining
    // in segments parks with the source still armed.
    m.tx_room = 8;
    EXPECT_EQ(kos_uart_write(&dev, six, 2), 2u) << "a call that fits is taken whole";
    EXPECT_EQ(m.tx_armed, 0u) << "a whole call disarmed the TX source";
    EXPECT_EQ(kos_uart_write(&dev, six, 0), 0u) << "a zero-length write is legal";
    EXPECT_EQ(m.tx_armed, 0u) << "a zero-length write leaves the source disarmed";
    EXPECT_EQ(m.tx_len, 6u) << "the device took six bytes across the three calls";

    // The class does NOT touch tx_bytes: the producer that queued the bytes owns that
    // counter, and a second writer breaks the shared block's single-writer rule.
    EXPECT_EQ(kos_counter_load(&stats.tx_bytes), 0u)
        << "write does not touch tx_bytes";
}

TEST(UartClass, read_reports_and_counts_what_arrived)
{
    struct kos_uart_mock m = {};
    m.rate = 115199;
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    configure(&cfg, &m, &stats);
    struct kos_uart dev;
    ASSERT_EQ(kos_uart_open(&dev, &cfg), 115199) << "open succeeded";

    m.rx[0] = 'R';
    m.rx[1] = 'X';
    m.rx[2] = '!';
    m.rx_len = 3;
    unsigned char in[8];
    EXPECT_EQ(kos_uart_read(&dev, in, 2), 2u) << "read is capped by the caller's length";
    EXPECT_TRUE(in[0] == 'R' and in[1] == 'X') << "read delivered the first two bytes";
    EXPECT_EQ(kos_uart_read(&dev, in, sizeof(in)), 1u) << "read returns the remainder";
    EXPECT_EQ(in[0], '!') << "read delivered the last byte";
    EXPECT_EQ(kos_counter_load(&stats.rx_bytes), 3u)
        << "read counted every delivered byte";
}

TEST(UartClass, flush_then_close_is_idempotent)
{
    struct kos_uart_mock m = {};
    m.rate = 115199;
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = {};
    configure(&cfg, &m, &stats);
    struct kos_uart dev;
    ASSERT_EQ(kos_uart_open(&dev, &cfg), 115199) << "open succeeded";

    // FLUSH BEFORE CLOSE is the contract's ordering, and close is idempotent.
    EXPECT_EQ(kos_uart_flush(&dev), 0) << "flush succeeds";
    EXPECT_EQ(kos_uart_close(&dev), 0) << "close succeeds";
    EXPECT_EQ(kos_uart_close(&dev), 0) << "a second close succeeds";
    EXPECT_EQ(m.flushes, 1u) << "flush reached the device once";
    EXPECT_EQ(m.closes, 2u) << "both closes reached the device";
}
