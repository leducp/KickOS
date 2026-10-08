// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The console endpoint's FRAMED arm, against the packaged one-thread simcon driver.
//
// Every arm below is the same assertion: the call RETURNS. A console endpoint carries two
// protocols, and a driver that does not tell a kos_call from a plain send leaves the caller
// parked forever on a reply that cannot come.
//
// The framed WRITE's payload is the one line the gate matches apart from the TAP stream.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>
#include <kickos/sys/console_ring.h> // stats_unpack

#include <stdint.h>

#include "tap.h"

namespace
{
    // Cap 0 is this thread's stdout: the console endpoint the driver published.
    constexpr int CH_EP = 0;

    // Clear of kos_uart_op, so a future op cannot turn this into a valid request.
    constexpr uint8_t OP_BOGUS = 200;

    // A WHOLE line, newline included, so the gate can match it on its own.
    char const wire_line[] = "[conabi] framed payload on the wire\n";
    constexpr uint16_t WIRE_LEN = sizeof(wire_line) - 1; // no NUL on the wire

    // The reply header's status, with its len apart: kos_uart_call folds the two into one number.
    int uart_status(uint8_t* buf, uint8_t op, uint8_t flags, uint16_t len, size_t carried,
                    uint16_t* got)
    {
        int const rc = kos_uart_call_in_place(CH_EP, buf, op, flags, len, carried);
        if (rc < 0)
        {
            return rc;
        }
        struct kos_uart_rsp rsp;
        uint8_t* dp = reinterpret_cast<uint8_t*>(&rsp);
        for (size_t i = 0; i < sizeof(rsp); i++)
        {
            dp[i] = buf[i];
        }
        *got = rsp.len;
        return rsp.status;
    }

    void t_set_mode_nonblock()
    {
        uint8_t buf[KOS_EP_MSG_MAX];
        uint16_t got = 0;
        TAP_CHECK(uart_status(buf, KOS_UART_SET_MODE, KOS_UART_F_NONBLOCK, 0, 0, &got) == 0);
    }

    // A host fd write always completes, so a request to block is satisfiable.
    void t_set_mode_blocking()
    {
        uint8_t buf[KOS_EP_MSG_MAX];
        uint16_t got = 0;
        TAP_CHECK(uart_status(buf, KOS_UART_SET_MODE, 0, 0, 0, &got) == 0);
    }

    void t_set_mode_bad_bit()
    {
        TAP_CHECK(kos_uart_call(CH_EP, KOS_UART_SET_MODE, 0x80, 0, nullptr, nullptr, 0)
                  == -KOS_EINVAL);
    }

    // The refusal has to be explicit: a 0-byte reply would read as "nothing yet".
    void t_read_refused()
    {
        TAP_CHECK(kos_uart_call(CH_EP, KOS_UART_READ, 0, 4, nullptr, nullptr, 0)
                  == -KOS_ENOSYS);
    }

    void t_configure_refused()
    {
        TAP_CHECK(kos_uart_call(CH_EP, KOS_UART_CONFIGURE, 0, 0, nullptr, nullptr, 0)
                  == -KOS_ENOSYS);
    }

    void t_bogus_op_refused()
    {
        TAP_CHECK(kos_uart_call(CH_EP, OP_BOGUS, 0, 0, nullptr, nullptr, 0) == -KOS_EINVAL);
    }

    // A frame claiming more payload than it carried is refused rather than read past.
    void t_short_frame_refused()
    {
        TAP_CHECK(kos_uart_call(CH_EP, KOS_UART_WRITE, 0, 64, nullptr, nullptr, 0)
                  == -KOS_EINVAL);
    }

    void t_framed_write()
    {
        uint8_t buf[KOS_EP_MSG_MAX];
        for (uint16_t i = 0; i < WIRE_LEN; i++)
        {
            buf[sizeof(struct kos_uart_req) + i] = static_cast<uint8_t>(wire_line[i]);
        }
        uint16_t took = 0;
        TAP_CHECK(uart_status(buf, KOS_UART_WRITE, 0, WIRE_LEN, WIRE_LEN, &took) == 0);
        TAP_CHECK(took == WIRE_LEN);
    }

    // The floor is the framed write alone; the TAP stream reaches the same driver as
    // plain sends.
    void t_stats()
    {
        uint8_t buf[KOS_EP_MSG_MAX];
        uint16_t stlen = 0;
        TAP_CHECK(uart_status(buf, KOS_UART_STATS, 0, 0, 0, &stlen) == 0);
        TAP_CHECK(stlen == sizeof(struct kos_uart_stats));
        struct kos_uart_stats s;
        kickos::console::stats_unpack(&s, buf + sizeof(struct kos_uart_rsp));
        TAP_CHECK(kos_counter_load(&s.tx_bytes) >= WIRE_LEN);
    }
}

int main(int, char**)
{
    tap::add("set_mode_nonblock", t_set_mode_nonblock);
    tap::add("set_mode_blocking", t_set_mode_blocking);
    tap::add("set_mode_bad_bit", t_set_mode_bad_bit);
    tap::add("read_refused", t_read_refused);
    tap::add("configure_refused", t_configure_refused);
    tap::add("bogus_op_refused", t_bogus_op_refused);
    tap::add("short_frame_refused", t_short_frame_refused);
    tap::add("framed_write", t_framed_write);
    tap::add("stats", t_stats);
    return tap::run_all();
}
