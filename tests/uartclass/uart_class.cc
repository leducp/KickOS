// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Contract gate for the raw UART class <kickos/driver/uart.h>: proves the five-call contract
// (open, read, write, flush, close) holds for a substituted backend, uart_mock.cc.

#include <kickos/driver/uart.h>

#include <kickos/sys/errno.h>

#include "uart_mock.h"

#include <stdint.h>
#include <stdio.h>

namespace
{
    int g_failures = 0;

    void check(bool ok, char const* what)
    {
        if (ok)
        {
            return;
        }
        printf("not ok - %s\n", what);
        g_failures++;
    }

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

    void case_open_refuses_an_unexpressible_frame()
    {
        struct kos_uart_mock m = {};
        m.rate = 115199;
        struct kos_uart_stats stats = {};
        struct kos_uart_config cfg = {};
        configure(&cfg, &m, &stats);

        // Refused BEFORE it binds: a consumer that ignored the return must not find a
        // half-open channel.
        struct kos_uart_config bad = cfg;
        bad.data_bits = 9;
        struct kos_uart dev;
        check(kos_uart_open(&dev, &bad) == -KOS_ENOTSUP, "a 9-bit frame is refused");
        check(m.opened == 0, "a refused open did not bind the channel");
    }

    void case_open_reports_a_measured_rate()
    {
        struct kos_uart_mock m = {};
        m.rate = 115199; // what the divider produces, never what a caller asks for
        struct kos_uart_stats stats = {};
        struct kos_uart_config cfg = {};
        configure(&cfg, &m, &stats);

        struct kos_uart dev;
        int32_t const rate = kos_uart_open(&dev, &cfg);
        check(rate == 115199, "open reports the modelled rate");
        check(rate != static_cast<int32_t>(cfg.baud), "open does not echo the request");
        check(m.opened == 1, "open bound the channel once");
    }

    void case_open_refuses_an_unknowable_rate()
    {
        // No divisor to read back: a backend that cannot MEASURE the rate must refuse.
        struct kos_uart_mock m = {};
        m.rate = 0;
        struct kos_uart_stats stats = {};
        struct kos_uart_config cfg = {};
        configure(&cfg, &m, &stats);

        struct kos_uart dev;
        check(kos_uart_open(&dev, &cfg) == -KOS_ENOSYS, "an unmeasurable rate is refused");
    }

    void case_read_on_an_idle_device_is_zero()
    {
        struct kos_uart_mock m = {};
        m.rate = 115199;
        struct kos_uart_stats stats = {};
        struct kos_uart_config cfg = {};
        configure(&cfg, &m, &stats);
        struct kos_uart dev;
        check(kos_uart_open(&dev, &cfg) == 115199, "open succeeded");

        // Nothing pending is 0, not an error: read is called on every pass even when the
        // wake was a doorbell, because it is also where the error latches get cleared.
        unsigned char in[8];
        check(kos_uart_read(&dev, in, sizeof(in)) == 0, "an idle read returns 0");
        check(stats.rx_bytes == 0, "an idle read counted nothing");
    }

    void case_write_is_short_and_owns_the_tx_arm()
    {
        struct kos_uart_mock m = {};
        m.rate = 115199;
        m.tx_room = 4;
        struct kos_uart_stats stats = {};
        struct kos_uart_config cfg = {};
        configure(&cfg, &m, &stats);
        struct kos_uart dev;
        check(kos_uart_open(&dev, &cfg) == 115199, "open succeeded");

        // A SHORT WRITE IS THE ORDINARY CASE: the count is what the device took, and the
        // refusal is what arms the TX source.
        unsigned char const six[6] = {'a', 'b', 'c', 'd', 'e', 'f'};
        check(kos_uart_write(&dev, six, 6) == 4, "write returns what the device took");
        check(m.tx_len == 4, "the device holds the accepted bytes");
        check(m.tx_armed == 1, "a refused byte armed the TX source");

        // A call the device took WHOLE disarms it, and that includes a zero-length call:
        // draining a queue in segments has to end on a call that was not refused, or the
        // consumer parks with the source still armed.
        m.tx_room = 8;
        check(kos_uart_write(&dev, six, 2) == 2, "a call that fits is taken whole");
        check(m.tx_armed == 0, "a whole call disarmed the TX source");
        check(kos_uart_write(&dev, six, 0) == 0, "a zero-length write is legal");
        check(m.tx_armed == 0, "a zero-length write leaves the source disarmed");
        check(m.tx_len == 6, "the device took six bytes across the three calls");

        // The class does NOT touch tx_bytes: the producer that queued the bytes owns that
        // counter, and a second writer would break the shared block's single-writer rule.
        check(stats.tx_bytes == 0, "write does not touch tx_bytes");
    }

    void case_read_reports_and_counts_what_arrived()
    {
        struct kos_uart_mock m = {};
        m.rate = 115199;
        struct kos_uart_stats stats = {};
        struct kos_uart_config cfg = {};
        configure(&cfg, &m, &stats);
        struct kos_uart dev;
        check(kos_uart_open(&dev, &cfg) == 115199, "open succeeded");

        m.rx[0] = 'R';
        m.rx[1] = 'X';
        m.rx[2] = '!';
        m.rx_len = 3;
        unsigned char in[8];
        check(kos_uart_read(&dev, in, 2) == 2, "read is capped by the caller's length");
        check(in[0] == 'R' and in[1] == 'X', "read delivered the first two bytes");
        check(kos_uart_read(&dev, in, sizeof(in)) == 1, "read returns the remainder");
        check(in[0] == '!', "read delivered the last byte");
        check(stats.rx_bytes == 3, "read counted every delivered byte");
    }

    void case_flush_then_close_is_idempotent()
    {
        struct kos_uart_mock m = {};
        m.rate = 115199;
        struct kos_uart_stats stats = {};
        struct kos_uart_config cfg = {};
        configure(&cfg, &m, &stats);
        struct kos_uart dev;
        check(kos_uart_open(&dev, &cfg) == 115199, "open succeeded");

        // FLUSH BEFORE CLOSE is the contract's ordering, and close is idempotent: a
        // consumer handing the channel on may run the pair twice.
        check(kos_uart_flush(&dev) == 0, "flush succeeds");
        check(kos_uart_close(&dev) == 0, "close succeeds");
        check(kos_uart_close(&dev) == 0, "a second close succeeds");
        check(m.flushes == 1, "flush reached the device once");
        check(m.closes == 2, "both closes reached the device");
    }

    // The kos_uart_cfg_check clauses, measured through the class entry point rather than by
    // calling the helper, so a backend that dropped the call fails here.
    void case_open_refuses_a_malformed_config()
    {
        struct kos_uart_mock m = {};
        m.rate = 115199;
        struct kos_uart_stats stats = {};
        struct kos_uart dev;

        struct kos_uart_config nobase = {};
        configure(&nobase, &m, &stats);
        nobase.base = 0u;
        check(kos_uart_open(&dev, &nobase) == -KOS_EINVAL, "a zero base is refused");

        struct kos_uart_config nostats = {};
        configure(&nostats, &m, &stats);
        nostats.stats = nullptr;
        check(kos_uart_open(&dev, &nostats) == -KOS_EINVAL, "a null stats block is refused");

        struct kos_uart_config dirty_rsv = {};
        configure(&dirty_rsv, &m, &stats);
        dirty_rsv.rsv = 1u;
        check(kos_uart_open(&dev, &dirty_rsv) == -KOS_EINVAL, "a non-zero rsv is refused");

        check(m.opened == 0, "no refused open bound the channel");
    }
}

int main()
{
    case_open_refuses_a_malformed_config();
    case_open_refuses_an_unexpressible_frame();
    case_open_refuses_an_unknowable_rate();
    case_open_reports_a_measured_rate();
    case_read_on_an_idle_device_is_zero();
    case_write_is_short_and_owns_the_tx_arm();
    case_read_reports_and_counts_what_arrived();
    case_flush_then_close_is_idempotent();

    if (g_failures != 0)
    {
        printf("FAIL: %d UART class contract check(s) failed\n", g_failures);
        return 1;
    }
    printf("ok - raw UART class contract (open, read, write, flush, close)\n");
    return 0;
}
