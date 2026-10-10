// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/uart_service.h>

#include <kickos/sys/errno.h>

#include "kos_seam.h"

#include <gtest/gtest.h>

#include <string.h>

namespace uart = kickos::uart;

namespace
{
    alignas(KICKOS_UART_BLOCK_SIZE) uart::Shared g_sh;
    uint8_t g_msg[KOS_EP_MSG_MAX];
}

extern "C"
{
    int32_t kos_call(kos_cap_t, void* buf, size_t send_len, size_t)
    {
        return static_cast<int32_t>(
            uart::serve_one(&g_sh, &g_sh.mode, static_cast<uint8_t*>(buf), send_len));
    }
}

namespace
{
    uint8_t* payload()
    {
        return g_msg + sizeof(struct kos_uart_req);
    }

    uint8_t const* reply()
    {
        return g_msg + sizeof(struct kos_uart_rsp);
    }

    int call(uint8_t op, uint8_t flags, uint16_t len, size_t carried)
    {
        return kos_uart_call_in_place(2, g_msg, op, flags, len, carried);
    }

    void fresh()
    {
        kos_seam_reset();
        uart::shared_init(&g_sh);
        memset(g_msg, 0, sizeof(g_msg));
    }

    TEST(UartServe, writes_are_accepted_whole_and_counted_in_the_stats)
    {
        fresh();
        memcpy(payload(), "hi!\n", 4);
        EXPECT_EQ(call(KOS_UART_WRITE, 0, 4, 4), 4);
        memset(payload(), 'x', 200);
        EXPECT_EQ(call(KOS_UART_WRITE, 0, 200, 200), 200);
        ASSERT_EQ(call(KOS_UART_STATS, 0, 0, 0), static_cast<int>(sizeof(struct kos_uart_stats)));
        struct kos_uart_stats s;
        kickos::console::stats_unpack(&s, reply());
        EXPECT_EQ(kos_counter_load(&s.tx_bytes), 204u);
    }

    TEST(UartServe, a_write_claiming_more_than_its_frame_carries_is_refused)
    {
        fresh();
        EXPECT_EQ(call(KOS_UART_WRITE, 0, 8, 0), -KOS_EINVAL);
    }

    TEST(UartServe, a_blocking_read_is_refused_not_answered_empty)
    {
        fresh();
        unsigned char const rx[4] = {'R', 'X', 'o', 'k'};
        ASSERT_EQ(kos_byte_ring_push(&g_sh.rx, rx, 4), 4u);
        EXPECT_EQ(call(KOS_UART_READ, KOS_UART_F_BLOCK, 4, 0), -KOS_ENOSYS);
    }

    TEST(UartServe, a_read_returns_the_bytes_of_the_rx_ring)
    {
        fresh();
        unsigned char const rx[4] = {'R', 'X', 'o', 'k'};
        ASSERT_EQ(kos_byte_ring_push(&g_sh.rx, rx, 4), 4u);
        ASSERT_EQ(call(KOS_UART_READ, 0, 4, 0), 4);
        EXPECT_EQ(memcmp(reply(), "RXok", 4), 0);
    }

    TEST(UartServe, set_mode_refuses_an_unknown_bit_and_stores_the_flag_it_accepts)
    {
        fresh();
        EXPECT_EQ(call(KOS_UART_SET_MODE, 0x80, 0, 0), -KOS_EINVAL);
        EXPECT_EQ(call(KOS_UART_SET_MODE, 0, 0, 0), 0);
        EXPECT_EQ(call(KOS_UART_SET_MODE, KOS_UART_F_NONBLOCK, 0, 0), 0);
        EXPECT_EQ(g_sh.mode, static_cast<uint32_t>(KOS_UART_F_NONBLOCK));
    }
}
