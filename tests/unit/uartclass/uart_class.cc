// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The refusals every backend of the raw UART class <kickos/driver/uart.h> shares, stated once in
// the contract's header so no backend grows its own dialect of them.

#include <kickos/driver/uart.h>

#include <kickos/sys/errno.h>

#include <stdint.h>

#include <gtest/gtest.h>

namespace
{
    struct kos_uart_config well_formed(struct kos_uart_stats* stats)
    {
        struct kos_uart_config cfg = {};
        cfg.base = 0x40000000u;
        cfg.stats = stats;
        cfg.baud = 115200;
        cfg.data_bits = 8;
        cfg.parity = KOS_UART_PARITY_NONE;
        cfg.stop_bits = 1;
        return cfg;
    }
}

TEST(UartClass, open_refuses_a_malformed_config)
{
    struct kos_uart_stats stats = {};
    struct kos_uart_config const cfg = well_formed(&stats);
    EXPECT_EQ(kos_uart_cfg_check(&cfg), 0) << "a well-formed config passes";

    struct kos_uart_config nobase = cfg;
    nobase.base = 0u;
    EXPECT_EQ(kos_uart_cfg_check(&nobase), -KOS_EINVAL) << "a zero base is refused";

    struct kos_uart_config nostats = cfg;
    nostats.stats = nullptr;
    EXPECT_EQ(kos_uart_cfg_check(&nostats), -KOS_EINVAL) << "a null stats block is refused";

    struct kos_uart_config dirty_rsv = cfg;
    dirty_rsv.rsv = 1u;
    EXPECT_EQ(kos_uart_cfg_check(&dirty_rsv), -KOS_EINVAL) << "a non-zero rsv is refused";
}

// The refusal value is THE CONTRACT'S, not a backend's.
TEST(UartClass, open_refuses_a_rate_request_it_cannot_program)
{
    struct kos_uart_stats stats = {};
    struct kos_uart_config cfg = well_formed(&stats);
    EXPECT_EQ(kos_uart_cfg_check_fixed_rate(&cfg), -KOS_ENOTSUP) << "a rate request is refused";

    // baud == 0 is the ONE request such a backend serves.
    cfg.baud = 0;
    EXPECT_EQ(kos_uart_cfg_check_fixed_rate(&cfg), 0) << "adopting the running rate is served";
}
