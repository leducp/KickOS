// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Sustained-output witness for a USB CDC console. Pushes several times the TX ring's
// depth through the published console endpoint and reports what the driver accepted, so
// a channel that wedges on a full ring is a NUMBER rather than a hang.
//
// The request path must stay a kos_call carrying a <kickos/sys/uart.h> frame: only the
// call reports a SHORT ACCEPT, and back-pressure is the whole point. A plain send's
// overflow is silently counted as tx_dropped by the driver instead. A cap 0 that refuses
// the call is reported as itself, never as a measurement that cannot fail.
//
// Every line goes to the console under test. On a board whose kernel console is a
// different peripheral the pin UART is dark from the handover on, so silence there plus
// no [rpusb] tag means bring-up SUCCEEDED and the output is on the CDC tty.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/uart.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

namespace
{
    // Cap 0 is this thread's stdout: the console endpoint the driver published.
    constexpr int CH_EP = 0;

    // Eight laps of the 1024-byte TX ring, so it reaches FULL repeatedly and the client is
    // forced onto the short-accept retry path. One lap would not reach it.
    constexpr uint32_t TOTAL = 8192;
    constexpr uint32_t CHUNK = 200;
    // A zero accept is ordinary back-pressure; only a RUN of them with no progress is a
    // stall. Each carries a 0.2 ms sleep, so this bound is ~400 ms, ~400 USB frames.
    constexpr uint32_t STALL_MAX = 2000;

    int uart_call(uint8_t op, uint16_t len, unsigned char const* payload,
                  unsigned char* out, unsigned out_max)
    {
        unsigned char buf[KOS_EP_MSG_MAX];
        struct kos_uart_req req;
        req.op = op;
        req.flags = 0;
        req.len = len;
        unsigned char const* rp = reinterpret_cast<unsigned char const*>(&req);
        for (unsigned i = 0; i < sizeof(req); i++)
        {
            buf[i] = rp[i];
        }
        unsigned send_len = sizeof(req);
        if (payload != nullptr)
        {
            for (uint16_t i = 0; i < len; i++)
            {
                buf[sizeof(req) + i] = payload[i];
            }
            send_len += len;
        }
        long const rc = kos_call(CH_EP, buf, send_len, sizeof(buf));
        if (rc < 0)
        {
            return static_cast<int>(rc);
        }
        struct kos_uart_rsp rsp;
        unsigned char* dp = reinterpret_cast<unsigned char*>(&rsp);
        for (unsigned i = 0; i < sizeof(rsp); i++)
        {
            dp[i] = buf[i];
        }
        if (rsp.status < 0)
        {
            return rsp.status;
        }
        if (out != nullptr and rsp.len <= out_max)
        {
            for (uint16_t i = 0; i < rsp.len; i++)
            {
                out[i] = buf[sizeof(rsp) + i];
            }
        }
        return static_cast<int>(rsp.len);
    }

    void say(char const* s)
    {
        kos::print(s);
    }
}

int main(int, char**)
{
    say("\n[usbcdcwit] start\n");

    unsigned char chunk[CHUNK];
    for (uint32_t i = 0; i < CHUNK; i++)
    {
        chunk[i] = static_cast<unsigned char>('a' + (i % 26));
    }

    // Probed before the loop: a refused cap must be reported as itself, not counted as a
    // stalled channel.
    int const probe = uart_call(KOS_UART_STATS, 0, nullptr, nullptr, 0);
    if (probe < 0)
    {
        char b[80];
        ksnprintf(b, sizeof(b), "[usbcdcwit] FAIL cap 0 refused the call: %d\n", probe);
        say(b);
        return 1;
    }

    uint32_t sent = 0;
    uint32_t zeros = 0;
    uint32_t max_zero_run = 0;
    int err = 0;
    while (sent < TOTAL)
    {
        uint32_t want = TOTAL - sent;
        if (want > CHUNK)
        {
            want = CHUNK;
        }
        int const took = uart_call(KOS_UART_WRITE, static_cast<uint16_t>(want), chunk,
                                   nullptr, 0);
        if (took < 0)
        {
            err = took;
            break;
        }
        sent += static_cast<uint32_t>(took);
        if (took > 0)
        {
            zeros = 0;
            continue;
        }
        zeros++;
        if (zeros > max_zero_run)
        {
            max_zero_run = zeros;
        }
        if (zeros >= STALL_MAX)
        {
            break;
        }
        kos_sleep_ns(200000ull); // 0.2 ms, well under a USB frame
    }

    unsigned char st[sizeof(struct kos_uart_stats)];
    struct kos_uart_stats stats;
    unsigned char* sp = reinterpret_cast<unsigned char*>(&stats);
    for (unsigned i = 0; i < sizeof(stats); i++)
    {
        sp[i] = 0;
    }
    if (uart_call(KOS_UART_STATS, 0, nullptr, st, sizeof(st))
        == static_cast<int>(sizeof(st)))
    {
        for (unsigned i = 0; i < sizeof(stats); i++)
        {
            sp[i] = st[i];
        }
    }

    char b[128];
    ksnprintf(b, sizeof(b), "\n[usbcdcwit] accepted=%u of %u err=%d maxzero=%u\n",
              static_cast<unsigned>(sent), static_cast<unsigned>(TOTAL), err,
              static_cast<unsigned>(max_zero_run));
    say(b);
    ksnprintf(b, sizeof(b), "[usbcdcwit] tx=%u drop=%u used=%u wakes=%u spurious=%u\n",
              static_cast<unsigned>(stats.tx_bytes),
              static_cast<unsigned>(stats.tx_dropped),
              static_cast<unsigned>(stats.tx_bytes - stats.tx_dropped),
              static_cast<unsigned>(stats.irq_wakes),
              static_cast<unsigned>(stats.irq_spurious));
    say(b);

    if (err != 0 or sent != TOTAL)
    {
        say("[usbcdcwit] FAIL (the channel stopped and did not recover)\n");
        return 1;
    }
    say("[usbcdcwit] PASS (sustained output past a full ring)\n");
    return 0;
}
