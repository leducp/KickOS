// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// End-to-end exercise of the buffered userspace UART: a client drives the
// <kickos/sys/uart.h> wire ABI against the REAL two-thread driver (not a mock), over the
// sim's loopback "device" (system/init/sim/service_list_uart.cc). What the run proves,
// beyond the mock-driven selftest case:
//
//   - the TX doorbell crosses threads: nothing but the service thread's kos_irq_notify
//     raises the line, so only that wake of the IRQ thread can move a byte;
//   - both rings stay SPSC across a real preemptible boundary, one writer per index and
//     each in a different thread;
//   - the loopback returns exactly what was sent, in order, so a mask or wrap bug in the
//     ring shows up as wrong CONTENT rather than a wrong count;
//   - the service thread was spawned with no window at all, so it cannot reach a register
//     even by mistake.
//
// The client must be a spawned thread: a kos_call parks its caller, and root has to stay
// alive to report the verdict.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

extern "C" kos_cap_t kickos_sim_uart_take_endpoint(void);

namespace
{
    constexpr int CH_DONE = 1; // delegated completion sem
    constexpr int CH_EP = 2;   // delegated SIGNAL-only cap on the service endpoint

    kos_cap_t g_done = KOS_CAP_NONE;
    // Root is the only reader and reads after the CH_DONE handshake, so no barrier beyond
    // the semaphore is needed.
    volatile int g_wrote = -99;
    volatile int g_read = -99;
    volatile int g_match = -99;
    volatile int g_wakes = -99;
    volatile int g_sustained = -99; // bytes accepted by the SUSTAINED arm, or a negative error

    // Several laps of the 512-byte TX ring, so it reaches FULL repeatedly and the client
    // is forced onto the short-accept retry path. One lap would not reach it, and the
    // defect this arm exists for only appears once a write is refused outright.
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

    // The short payload never fills the ring, so only this arm can catch a producer that
    // stops ringing the doorbell once the ring is full: the consumer is parked and the
    // producer is the only thing that can wake it, so one silent zero-accept ends the
    // stream for good. Reported as bytes ACCEPTED, so a wedged channel fails the gate as
    // a number instead of timing it out.
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
                // Drain the loopback's RX ring so that IT is never what saturates: this
                // arm is about the TX path.
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
        // thread's drain, so the read has to be polled rather than read once. The retry is
        // bounded so a genuine failure fails the gate instead of hanging it.
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

        // AFTER the content check, never before: this leaves the rings deliberately
        // backed up, which would make the ordered-loopback assertion above meaningless.
        sustain();

        unsigned char st[sizeof(struct kos_uart_stats)];
        if (uart_call(KOS_UART_STATS, 0, 0, nullptr, st, sizeof(st))
            == static_cast<int>(sizeof(st)))
        {
            struct kos_uart_stats s;
            unsigned char* dp = reinterpret_cast<unsigned char*>(&s);
            for (unsigned i = 0; i < sizeof(s); i++)
            {
                dp[i] = st[i];
            }
            g_wakes = static_cast<int>(s.irq_wakes);
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
    auto const cl = kos::thread::spawn_caps(client, nullptr, "uartcl", 10, caps, 2);
    if (not cl.valid())
    {
        kos_print("[uartloop] ERROR: client spawn failed\n");
        return 1;
    }
    kos_sem_wait(g_done);

    int const n = payload_len();
    char line[96];
    ksnprintf(line, sizeof(line), "[uartloop] wrote=%d read=%d match=%d wakes=%d\n",
              g_wrote, g_read, g_match, g_wakes);
    kos_print(line);
    ksnprintf(line, sizeof(line), "[uartloop] sustained=%d of %u\n", g_sustained,
              static_cast<unsigned>(SUSTAIN_TOTAL));
    kos_print(line);
    if (g_wrote != n or g_read != n or g_match != 1)
    {
        kos_print("[uartloop] FAIL (loopback)\n");
        return 1;
    }
    if (g_sustained != static_cast<int>(SUSTAIN_TOTAL))
    {
        kos_print("[uartloop] FAIL (sustained: the channel stopped and did not recover)\n");
        return 1;
    }
    kos_print("[uartloop] PASS (loopback in order; sustained output past a full ring)\n");
    return 0;
}
