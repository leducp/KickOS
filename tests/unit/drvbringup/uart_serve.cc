// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/uart_service.h>

#include <kickos/sys/errno.h>
#include <kickos/sys/rights.h>

#include "kos_seam.h"

#include <gtest/gtest.h>

#include <string.h>

namespace uart = kickos::uart;

namespace
{
    alignas(KICKOS_UART_BLOCK_SIZE) uart::Shared g_sh;
    uint8_t g_msg[KOS_EP_MSG_MAX];
    uint32_t g_badge = KOS_RIGHT_WRITE | KOS_RIGHT_READ | KOS_RIGHT_CONTROL;
}

extern "C"
{
    int32_t kos_call(kos_cap_t, void* buf, size_t send_len, size_t, kos_monotonic_t)
    {
        return static_cast<int32_t>(
            kickos::console::serve_one<uart::Transport>(&g_sh, g_badge, static_cast<uint8_t*>(buf),
                                                        send_len));
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
        g_badge = KOS_RIGHT_WRITE | KOS_RIGHT_READ | KOS_RIGHT_CONTROL;
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

    TEST(UartServe, read_and_set_mode_need_their_rights)
    {
        fresh();
        unsigned char const rx[4] = {'R', 'X', 'o', 'k'};
        ASSERT_EQ(kos_byte_ring_push(&g_sh.rx, rx, 4), 4u);
        for (uint32_t const badge : {static_cast<uint32_t>(KOS_BADGE_NONE), static_cast<uint32_t>(KOS_RIGHT_WRITE)})
        {
            g_badge = badge;
            EXPECT_EQ(call(KOS_UART_READ, 0, 4, 0), -KOS_EACCES) << badge;
            EXPECT_EQ(call(KOS_UART_SET_MODE, KOS_UART_F_NONBLOCK, 0, 0), -KOS_EACCES) << badge;
        }
        EXPECT_EQ(g_sh.mode, 0u) << "no refused SET_MODE stored its flag";
        EXPECT_EQ(kos_byte_ring_used(&g_sh.rx), 4u) << "no refused READ took a byte";
        g_badge = KOS_RIGHT_READ;
        EXPECT_EQ(call(KOS_UART_READ, 0, 4, 0), 4);
        EXPECT_EQ(call(KOS_UART_SET_MODE, KOS_UART_F_NONBLOCK, 0, 0), -KOS_EACCES);
        g_badge = KOS_RIGHT_CONTROL;
        EXPECT_EQ(call(KOS_UART_SET_MODE, KOS_UART_F_NONBLOCK, 0, 0), 0);
    }

    TEST(UartServe, write_stats_and_drain_need_write)
    {
        fresh();
        g_badge = KOS_RIGHT_READ | KOS_RIGHT_CONTROL;
        memcpy(payload(), "hi", 2);
        EXPECT_EQ(call(KOS_UART_WRITE, 0, 2, 2), -KOS_EACCES);
        EXPECT_EQ(call(KOS_UART_STATS, 0, 0, 0), -KOS_EACCES);
        EXPECT_EQ(call(KOS_UART_DRAIN, 0, 0, 0), -KOS_EACCES);
        g_badge = KOS_RIGHT_WRITE;
        EXPECT_EQ(call(KOS_UART_WRITE, 0, 2, 2), 2);
    }

    TEST(UartServe, stats_show_receive_activity_only_to_a_reader)
    {
        fresh();
        unsigned char const rx[4] = {'R', 'X', 'o', 'k'};
        ASSERT_EQ(kos_byte_ring_push(&g_sh.rx, rx, 4), 4u);
        kos_counter_increment(&g_sh.stats.rx_bytes, 4u);
        kos_counter_increment(&g_sh.stats.irq_wakes, 2u);
        struct kos_uart_stats s;
        g_badge = KOS_RIGHT_WRITE;
        ASSERT_EQ(call(KOS_UART_STATS, 0, 0, 0), static_cast<int>(sizeof(struct kos_uart_stats)));
        kickos::console::stats_unpack(&s, reply());
        EXPECT_EQ(kos_counter_load(&s.rx_bytes), 0u) << "a writer saw the console's input";
        EXPECT_EQ(kos_counter_load(&s.irq_wakes), 0u) << "a writer saw the console's input timing";
        g_badge = KOS_RIGHT_WRITE | KOS_RIGHT_READ;
        ASSERT_EQ(call(KOS_UART_STATS, 0, 0, 0), static_cast<int>(sizeof(struct kos_uart_stats)));
        kickos::console::stats_unpack(&s, reply());
        EXPECT_EQ(kos_counter_load(&s.rx_bytes), 4u);
        EXPECT_EQ(kos_counter_load(&s.irq_wakes), 2u);
    }
}
