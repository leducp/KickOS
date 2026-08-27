// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// End-to-end exercise of the buffered userspace UART: a client drives the
// <kickos/sys/uart.h> wire ABI against the REAL two-thread driver, over the sim's loopback
// "device" (system/init/sim/service_list_uart.cc).
//
// The client must be a spawned thread: a kos_call parks its caller, and root has to stay
// alive to report the verdict.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>
#include <kickos/sys/console_ring.h> // stats_unpack
#include <kickos/libc/fmt.h>

#include <stdint.h>

extern "C" kos_cap_t kickos_sim_uart_take_endpoint(void);

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    constexpr int CH_DONE = 1; // delegated completion sem
    constexpr int CH_EP = 2;   // delegated SIGNAL-only cap on the service endpoint

    kos_cap_t g_done = KOS_CAP_NONE;
    // Read by root, only after the CH_DONE handshake.
    Atomic<int, Order::RELAXED> g_wrote{-99};
    Atomic<int, Order::RELAXED> g_read{-99};
    Atomic<int, Order::RELAXED> g_match{-99};
    Atomic<int, Order::RELAXED> g_wakes{-99};
    // bytes accepted by the SUSTAINED arm, or a negative error
    Atomic<int, Order::RELAXED> g_sustained{-99};

    // Several laps of the 512-byte TX ring, so it reaches FULL repeatedly and the client
    // is forced onto the short-accept retry path.
    constexpr uint32_t SUSTAIN_TOTAL = 4096;
    constexpr uint32_t SUSTAIN_CHUNK = 200;
    // A zero accept is normal back-pressure; only a RUN of them with no progress between
    // is a stall. Each carries a 1 ms sleep, so this bound is ~400 ms, far past the
    // modelled device's drain time for a full ring.
    constexpr uint32_t SUSTAIN_STALL_MAX = 400;

    char const* PAYLOAD = "KickOS UART loopback\n";

    int payload_len()
    {
        int n = 0;
        while (PAYLOAD[n] != '\0')
        {
            n++;
        }
        return n;
    }

    int uart_call(uint8_t op, uint8_t flags, uint16_t len, unsigned char const* payload,
                  unsigned char* out, uint16_t out_max)
    {
        unsigned char buf[KOS_EP_MSG_MAX];
        struct kos_uart_req req;
        for (unsigned i = 0; i < sizeof(req); i++)
        {
            reinterpret_cast<unsigned char*>(&req)[i] = 0;
        }
        req.op = op;
        req.flags = flags;
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

    // The consumer is parked and the producer is the only thing that can wake it, so a
    // producer that stops ringing the doorbell on a full ring ends the stream for good.
    // Reported as bytes ACCEPTED, so a wedged channel fails the gate as a number rather
    // than as a timeout.
    void sustain()
    {
        unsigned char chunk[SUSTAIN_CHUNK];
        for (uint32_t i = 0; i < SUSTAIN_CHUNK; i++)
        {
            chunk[i] = static_cast<unsigned char>('a' + (i % 26));
        }
        uint32_t sent = 0;
        uint32_t zeros = 0;
        while (sent < SUSTAIN_TOTAL)
        {
            uint32_t want = SUSTAIN_TOTAL - sent;
            if (want > SUSTAIN_CHUNK)
            {
                want = SUSTAIN_CHUNK;
            }
            int const took = uart_call(KOS_UART_WRITE, 0, static_cast<uint16_t>(want),
                                       chunk, nullptr, 0);
            if (took < 0)
            {
                g_sustained = took;
                return;
            }
            sent += static_cast<uint32_t>(took);
            if (took > 0)
            {
                zeros = 0;
                // Drain the loopback's RX ring so that IT is never what saturates.
                unsigned char sink[240];
                (void)uart_call(KOS_UART_READ, 0, 240, nullptr, sink, sizeof(sink));
                continue;
            }
            zeros++;
            if (zeros >= SUSTAIN_STALL_MAX)
            {
                g_sustained = static_cast<int>(sent);
                return;
            }
            kos_sleep_ns(1000000ull);
        }
        g_sustained = static_cast<int>(sent);
    }

    void client(void*)
    {
        int const n = payload_len();
        g_wrote = uart_call(KOS_UART_WRITE, 0, static_cast<uint16_t>(n),
                            reinterpret_cast<unsigned char const*>(PAYLOAD), nullptr, 0);

        // The write returns when the SERVICE thread replies, which can precede the IRQ
        // thread's drain, so the read must be polled. Bounded, so a genuine failure fails
        // the gate instead of hanging it.
        unsigned char back[64];
        int got = 0;
        for (int spin = 0; spin < 100 and got < n; spin++)
        {
            int const r = uart_call(KOS_UART_READ, 0, static_cast<uint16_t>(n - got),
                                    nullptr, back + got, static_cast<uint16_t>(64 - got));
            if (r < 0)
            {
                got = r;
                break;
            }
            got += r;
            if (got < n)
            {
                kos_sleep_ns(1000000ull);
            }
        }
        g_read = got;

        int match = 1;
        if (got == n)
        {
            for (int i = 0; i < n; i++)
            {
                if (back[i] != static_cast<unsigned char>(PAYLOAD[i]))
                {
                    match = 0;
                }
            }
        }
        else
        {
            match = 0;
        }
        g_match = match;

        // AFTER the content check, never before: it leaves the rings backed up, which
        // would make the ordered-loopback check above meaningless.
        sustain();

        unsigned char st[sizeof(struct kos_uart_stats)];
        if (uart_call(KOS_UART_STATS, 0, 0, nullptr, st, sizeof(st))
            == static_cast<int>(sizeof(st)))
        {
            struct kos_uart_stats s;
            kickos::console::stats_unpack(&s, st);
            g_wakes = static_cast<int>(kos_counter_load(&s.irq_wakes));
        }
        kos_sem_post(CH_DONE);
    }
}

KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM);

int main(int, char**)
{
    kos_cap_t const ep = kickos_sim_uart_take_endpoint();
    if (ep == KOS_CAP_NONE)
    {
        kos_print("[uartloop] ERROR: no UART service endpoint (wrong service list?)\n");
        return 1;
    }
    if (kos_sem_create(0, &g_done) != 0)
    {
        kos_print("[uartloop] ERROR: sem_create failed\n");
        return 1;
    }
    kos_cap_grant const caps[2] = {{g_done, KOS_CAP_WAIT | KOS_CAP_SIGNAL},
                                   {ep, KOS_CAP_SIGNAL}};
    auto const cl = kos::thread::create_caps(client, nullptr, "uartcl", 10, caps, 2);
    if (not cl.valid())
    {
        kos_print("[uartloop] ERROR: client spawn failed\n");
        return 1;
    }
    kos_sem_wait(g_done);

    int const n = payload_len();
    int const n_wrote = g_wrote;
    int const n_read = g_read;
    int const matched = g_match;
    int const sustained = g_sustained;
    char line[96];
    ksnprintf(line, sizeof(line), "[uartloop] wrote=%d read=%d match=%d wakes=%d\n",
              n_wrote, n_read, matched, g_wakes.load());
    kos_print(line);
    ksnprintf(line, sizeof(line), "[uartloop] sustained=%d of %u\n", sustained,
              static_cast<unsigned>(SUSTAIN_TOTAL));
    kos_print(line);
    if (n_wrote != n or n_read != n or matched != 1)
    {
        kos_print("[uartloop] FAIL (loopback)\n");
        return 1;
    }
    if (sustained != static_cast<int>(SUSTAIN_TOTAL))
    {
        kos_print("[uartloop] FAIL (sustained: the channel stopped and did not recover)\n");
        return 1;
    }
    kos_print("[uartloop] PASS (loopback in order; sustained output past a full ring)\n");
    return 0;
}
