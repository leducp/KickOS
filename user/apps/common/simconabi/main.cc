// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The console endpoint's FRAMED arm, exercised against a real published driver.
//
// Every arm below is the same assertion: the call RETURNS. A console endpoint carries two
// protocols, and a driver that does not tell a kos_call from a plain send leaves the caller
// parked forever on a reply that cannot come.
//
// Only kickos_services_sim's one-thread driver is covered here. Every other console service
// answers through uart_service.h's serve_one, which the selftest already drives.
//
// The framed WRITE's payload is the one line the gate matches apart from the TAP stream: it
// travels the console as BYTES rather than as a verdict about it.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>
#include <kickos/sys/console_ring.h> // stats_unpack

#include <atomic>
#include <stdint.h>

#include "tap.h"

namespace
{
    // Cap 0 is this thread's stdout: the console endpoint the driver published.
    constexpr int CH_EP = 0;

    // Clear of kos_uart_op, so a future op cannot turn this into a valid request.
    constexpr uint8_t OP_BOGUS = 200;

    // The reply's status, or the error the call failed with. A reply shorter than the
    // header is its own failure.
    int uart_call(uint8_t op, uint8_t flags, uint16_t len, uint8_t const* payload,
                  uint8_t* out, uint16_t out_max, uint16_t* got)
    {
        uint8_t buf[KOS_EP_MSG_MAX];
        struct kos_uart_req req;
        req.op = op;
        req.flags = flags;
        req.len = len;
        req.baud = 0;
        req.data_bits = 0;
        req.parity = 0;
        req.stop_bits = 0;
        req.rsv = 0;
        uint8_t const* rp = reinterpret_cast<uint8_t const*>(&req);
        for (size_t i = 0; i < sizeof(req); i++)
        {
            buf[i] = rp[i];
        }
        size_t send_len = sizeof(req);
        if (payload != nullptr)
        {
            for (uint16_t i = 0; i < len; i++)
            {
                buf[sizeof(req) + i] = payload[i];
            }
            send_len += len;
        }
        int32_t const rc = kos_call(CH_EP, buf, send_len, sizeof(buf));
        if (rc < 0)
        {
            return static_cast<int>(rc);
        }
        if (static_cast<size_t>(rc) < sizeof(struct kos_uart_rsp))
        {
            return -KOS_EINVAL;
        }
        struct kos_uart_rsp rsp;
        uint8_t* dp = reinterpret_cast<uint8_t*>(&rsp);
        for (size_t i = 0; i < sizeof(rsp); i++)
        {
            dp[i] = buf[i];
        }
        if (got != nullptr)
        {
            *got = rsp.len;
        }
        if (out != nullptr and rsp.status >= 0)
        {
            uint16_t n = rsp.len;
            if (n > out_max)
            {
                n = out_max;
            }
            for (size_t i = 0; i < n; i++)
            {
                out[i] = buf[sizeof(rsp) + i];
            }
        }
        return rsp.status;
    }

    // A WHOLE line, newline included, so the gate can match it on its own.
    char const wire_line[] = "[conabi] framed payload on the wire\n";
    constexpr uint16_t WIRE_LEN = sizeof(wire_line) - 1; // no NUL on the wire

    void t_set_mode_nonblock()
    {
        TAP_CHECK(uart_call(KOS_UART_SET_MODE, KOS_UART_F_NONBLOCK, 0, nullptr, nullptr, 0,
                            nullptr)
                  == 0);
    }

    // A host fd write always completes, so this transport requires nothing and a request to
    // block is satisfiable.
    void t_set_mode_blocking()
    {
        TAP_CHECK(uart_call(KOS_UART_SET_MODE, 0, 0, nullptr, nullptr, 0, nullptr) == 0);
    }

    void t_set_mode_bad_bit()
    {
        TAP_CHECK(uart_call(KOS_UART_SET_MODE, 0x80, 0, nullptr, nullptr, 0, nullptr)
                  == -KOS_EINVAL);
    }

    // No RX arm at all: a 0-byte reply would read as "nothing yet".
    void t_read_refused()
    {
        TAP_CHECK(uart_call(KOS_UART_READ, 0, 4, nullptr, nullptr, 0, nullptr)
                  == -KOS_ENOSYS);
    }

    void t_configure_refused()
    {
        TAP_CHECK(uart_call(KOS_UART_CONFIGURE, 0, 0, nullptr, nullptr, 0, nullptr)
                  == -KOS_ENOSYS);
    }

    void t_bogus_op_refused()
    {
        TAP_CHECK(uart_call(OP_BOGUS, 0, 0, nullptr, nullptr, 0, nullptr) == -KOS_EINVAL);
    }

    // A frame claiming more payload than it carried is refused rather than read past.
    void t_short_frame_refused()
    {
        TAP_CHECK(uart_call(KOS_UART_WRITE, 0, 64, nullptr, nullptr, 0, nullptr)
                  == -KOS_EINVAL);
    }

    void t_framed_write()
    {
        uint16_t took = 0;
        TAP_CHECK(uart_call(KOS_UART_WRITE, 0, WIRE_LEN,
                            reinterpret_cast<uint8_t const*>(wire_line), nullptr, 0, &took)
                  == 0);
        TAP_CHECK(took == WIRE_LEN);
    }

    // Live counters, not a zeroed struct answered to shut the caller up. The floor is the
    // framed write alone; the TAP stream reaches the same driver as plain sends.
    void t_stats()
    {
        uint8_t st[sizeof(struct kos_uart_stats)];
        uint16_t stlen = 0;
        TAP_CHECK(uart_call(KOS_UART_STATS, 0, 0, nullptr, st, sizeof(st), &stlen) == 0);
        TAP_CHECK(stlen == sizeof(struct kos_uart_stats));
        struct kos_uart_stats s;
        kickos::console::stats_unpack(&s, st);
        TAP_CHECK(s.tx_bytes.load(std::memory_order_relaxed) >= WIRE_LEN);
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
