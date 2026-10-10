// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/console_ring.h>

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <string.h>

#include <string>

namespace console = kickos::console;

namespace
{
    // The consumer a sleeping producer waits on: it empties the ring, so a write that
    // blocks returns whole instead of hanging the suite.
    struct kos_byte_ring* g_drained = nullptr;
    uint32_t g_sleeps = 0u;
}

extern "C"
{
    int kos_notify(kos_cap_t)
    {
        return 0;
    }

    void kos_sleep_ns(uint64_t)
    {
        g_sleeps++;
        if (g_drained != nullptr)
        {
            unsigned char sink[64];
            while (kos_byte_ring_pop(g_drained, sink, sizeof(sink)) != 0u)
            {
            }
        }
    }
}

namespace
{
    struct Cook
    {
        char const* name;
        char const* in;
        uint32_t room;
        uint32_t taken;
        char const* out;
    };

    class CookCrlf : public ::testing::TestWithParam<Cook>
    {
    };

    TEST_P(CookCrlf, cooks_its_input)
    {
        Cook const& c = GetParam();
        unsigned char out[16];
        uint32_t taken = 99;
        uint32_t const n = console::cook_crlf(reinterpret_cast<unsigned char const*>(c.in),
                                              static_cast<uint32_t>(strlen(c.in)), out, c.room, &taken);
        ASSERT_EQ(n, strlen(c.out));
        EXPECT_EQ(taken, c.taken);
        EXPECT_EQ(memcmp(out, c.out, n), 0);
    }

    INSTANTIATE_TEST_SUITE_P(
        Row, CookCrlf,
        ::testing::Values(Cook{"input_with_no_newline_is_copied_as_is", "abc", 16, 3, "abc"},
                          Cook{"every_newline_gains_a_carriage_return_before_it", "a\nb", 16, 3, "a\r\nb"},
                          Cook{"a_crlf_input_is_not_looked_back_on_and_becomes_cr_cr_lf", "\r\n", 16, 2, "\r\r\n"},
                          Cook{"a_newline_with_no_room_for_its_carriage_return_is_left_whole", "x\n", 2, 1, "x"},
                          Cook{"a_newline_left_whole_is_resumed_with_its_carriage_return", "\n", 2, 1, "\r\n"},
                          Cook{"no_output_room_consumes_no_input", "abc", 0, 0, ""}),
        [](::testing::TestParamInfo<Cook> const& p)
        {
            return std::string{p.param.name};
        });

    TEST(ModeApply, a_service_with_no_console_arm_refuses)
    {
        EXPECT_EQ(console::mode_apply(nullptr, KOS_UART_F_NONBLOCK, 0u), -KOS_ENOSYS);
    }

    TEST(ModeApply, an_unknown_bit_is_refused_whole_and_stores_nothing)
    {
        kickos::Atomic<uint32_t, kickos::Order::RELAXED> mode{0};
        EXPECT_EQ(console::mode_apply(&mode, 0x80u, 0u), -KOS_EINVAL);
        EXPECT_EQ(mode, 0u);
        EXPECT_EQ(console::mode_apply(&mode, 0x80u | KOS_UART_F_NONBLOCK, 0u), -KOS_EINVAL);
        EXPECT_EQ(mode, 0u);
    }

    TEST(ModeApply, the_flag_is_stored_and_cleared_again)
    {
        kickos::Atomic<uint32_t, kickos::Order::RELAXED> mode{0};
        EXPECT_EQ(console::mode_apply(&mode, KOS_UART_F_NONBLOCK, 0u), 0);
        EXPECT_EQ(mode, static_cast<uint32_t>(KOS_UART_F_NONBLOCK));
        EXPECT_EQ(console::mode_apply(&mode, 0u, 0u), 0);
        EXPECT_EQ(mode, 0u);
    }

    TEST(ModeApply, a_required_bit_cannot_be_cleared_and_the_refusal_stores_nothing)
    {
        kickos::Atomic<uint32_t, kickos::Order::RELAXED> mode{KOS_UART_F_NONBLOCK};
        EXPECT_EQ(console::mode_apply(&mode, 0u, KOS_UART_F_NONBLOCK), -KOS_ENOTSUP);
        EXPECT_EQ(mode, static_cast<uint32_t>(KOS_UART_F_NONBLOCK));
        EXPECT_EQ(console::mode_apply(&mode, KOS_UART_F_NONBLOCK, KOS_UART_F_NONBLOCK), 0);
        EXPECT_EQ(mode, static_cast<uint32_t>(KOS_UART_F_NONBLOCK));
    }

    unsigned char const LINES[12] = {'a', '\n', 'b', '\n', 'c', '\n',
                                     'd', '\n', 'e', '\n', 'f', '\n'};

    TEST(WriteConsole, a_non_blocking_write_reports_its_short_accept_without_waiting)
    {
        unsigned char rbuf[8];
        struct kos_byte_ring ring;
        kos_byte_ring_init(&ring, rbuf, sizeof(rbuf));
        struct kos_uart_stats st = {};
        g_drained = &ring;
        g_sleeps = 0u;
        uint32_t const nb = console::write_console(&ring, &st, LINES, 12, KOS_UART_F_NONBLOCK);
        g_drained = nullptr;
        EXPECT_EQ(g_sleeps, 0u) << "a non-blocking write waited for room";
        EXPECT_LT(nb, 12u);
        uint32_t const cooked = kos_counter_load(&st.tx_bytes);
        EXPECT_GT(cooked, 0u);
        EXPECT_LE(cooked, static_cast<uint32_t>(sizeof(rbuf)));
        EXPECT_GE(cooked, nb);
    }

    TEST(WriteConsole, the_return_counts_input_bytes_and_tx_bytes_counts_wire_bytes)
    {
        unsigned char rbuf[32];
        struct kos_byte_ring ring;
        kos_byte_ring_init(&ring, rbuf, sizeof(rbuf));
        struct kos_uart_stats st = {};
        uint32_t const nb = console::write_console(&ring, &st, LINES, 12, KOS_UART_F_NONBLOCK);
        uint32_t const cooked = kos_counter_load(&st.tx_bytes);
        uint32_t added = 0u;
#if KICKOS_CONSOLE_CRLF
        added = 6u;
#endif
        EXPECT_EQ(nb, 12u);
        EXPECT_EQ(cooked - nb, added);
        unsigned char wire[32] = {0};
        EXPECT_EQ(kos_byte_ring_pop(&ring, wire, sizeof(wire)), cooked);
    }
}
