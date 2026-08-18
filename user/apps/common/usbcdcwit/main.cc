// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Sustained-output witness for a USB CDC console. Pushes several times the TX ring's
// depth through the published console endpoint and reports what the driver accepted, so
// a channel that wedges on a full ring is a NUMBER rather than a hang.
//
// The request path must stay a kos_call carrying a <kickos/sys/uart.h> frame: only the call
// reports a SHORT ACCEPT. A plain send's overflow is silently counted as tx_dropped.
//
// Every line goes out through STDIO, never `kos::print`: a userspace driver owns the
// console in every image this app runs in, and `console_emit` drops a kernel-console write
// in that state.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/uart.h>
#include <kickos/sys/console_ring.h> // stats_unpack
#include <kickos/libc/fmt.h>

#include <stdint.h>
#include <stdio.h>

// Emitted by cmake/build_stamp.cmake and carried by every image.
extern "C" char const kickos_build_commit[];

namespace
{
    // Cap 0 is this thread's stdout: the console endpoint the driver published.
    constexpr int CH_EP = 0;

    // Eight laps of the 1024-byte TX ring, so it reaches FULL repeatedly and the client is
    // forced onto the short-accept retry path. One lap would not reach it.
    constexpr uint32_t TOTAL = 8192;
    constexpr uint32_t CHUNK = 200;
    // A zero accept is ordinary back-pressure; only a RUN of them with no progress is a
    // stall. The bound is set by the HOST, not the wire: no IN token is issued until
    // something OPENS the tty, and that node appears about a second after boot, so a
    // budget under that reports an idle reader as a stalled channel.
    constexpr uint64_t RETRY_SLEEP_NS = 200000;  // 0.2 ms, well under a USB frame
    constexpr uint32_t STALL_BUDGET_MS = 2000;
    constexpr uint32_t STALL_MAX =
        static_cast<uint32_t>((STALL_BUDGET_MS * 1000000ull) / RETRY_SLEEP_NS);

    int uart_call(uint8_t op, uint16_t len, uint8_t const* payload,
                  uint8_t* out, uint16_t out_max)
    {
        uint8_t buf[KOS_EP_MSG_MAX];
        struct kos_uart_req req;
        req.op = op;
        req.flags = 0;
        req.len = len;
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
        struct kos_uart_rsp rsp;
        uint8_t* dp = reinterpret_cast<uint8_t*>(&rsp);
        for (size_t i = 0; i < sizeof(rsp); i++)
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

    // Flushed per line: the run ends in a reboot that takes the USB device away, and a
    // stdio buffer would go with it.
    void say(char const* s)
    {
        fputs(s, stdout);
        fflush(stdout);
    }

    // A zero-length plain send on the console endpoint means FLUSH. TWICE: a plain send is
    // released when the receiver TAKES it, so the first only starts the drain and the second
    // cannot be taken until the driver is back in kos_recv, past that drain.
    void drain_console()
    {
        (void)kos_send(CH_EP, "", 0);
        (void)kos_send(CH_EP, "", 0);
    }
}

int main(int, char**)
{
    say("\n[usbcdcwit] start\n");

    uint8_t chunk[CHUNK];
    for (uint32_t i = 0; i < CHUNK; i++)
    {
        chunk[i] = static_cast<uint8_t>('a' + (i % 26));
    }

    // A refused cap must be reported as itself, not counted as a stalled channel.
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
        kos_sleep_ns(RETRY_SLEEP_NS);
    }

    // Before the counters are read, so tx_dropped has stopped moving.
    drain_console();

    uint8_t st[sizeof(struct kos_uart_stats)];
    struct kos_uart_stats stats = {};
    if (uart_call(KOS_UART_STATS, 0, nullptr, st, sizeof(st))
        == static_cast<int>(sizeof(st)))
    {
        kickos::console::stats_unpack(&stats, st);
    }

    char b[128];
    // Nothing can listen on this transport until the image has booted, so the kernel
    // banner is lost from every capture. Reprinted here, `-dirty` included.
    ksnprintf(b, sizeof(b), "\n[usbcdcwit] commit %s\n", kickos_build_commit);
    say(b);
    ksnprintf(b, sizeof(b), "[usbcdcwit] accepted=%u of %u err=%d maxzero=%u\n",
              static_cast<unsigned>(sent), static_cast<unsigned>(TOTAL), err,
              static_cast<unsigned>(max_zero_run));
    say(b);
    // `queued` is bytes the ring TOOK and did not lose, NOT ring occupancy: the wire ABI
    // carries no field for that. Delivery is the HOST's byte count against `queued`.
    uint32_t const tx = kos_counter_load(&stats.tx_bytes);
    uint32_t const drop = kos_counter_load(&stats.tx_dropped);
    ksnprintf(b, sizeof(b), "[usbcdcwit] tx=%u drop=%u queued=%u wakes=%u spurious=%u\n",
              static_cast<unsigned>(tx), static_cast<unsigned>(drop),
              static_cast<unsigned>(tx - drop),
              static_cast<unsigned>(kos_counter_load(&stats.irq_wakes)),
              static_cast<unsigned>(kos_counter_load(&stats.irq_spurious)));
    say(b);

    if (err != 0 or sent != TOTAL)
    {
        say("[usbcdcwit] FAIL (the channel stopped and did not recover)\n");
        return 1;
    }
    say("[usbcdcwit] PASS (sustained output past a full ring)\n");
    return 0;
}
