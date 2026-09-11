// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The REQUEST side of a published console: the <kickos/sys/uart.h> op dispatch and the
// two-protocol recv loop. The ring side is <kickos/sys/console_ring.h>; the rules, the
// budgets and the CRLF posture are stated there. No register and no device class.
//
// A Transport is a plain struct of constants and static functions naming what a transport
// changes about these two bodies, and nothing else:
//     static constexpr uint32_t MODE_REQUIRED;
//         Mode bits the transport cannot clear, which is also the mode shared_init seats:
//         the minimum an endpoint can be opened at is what it is never allowed to leave.
//     static Atomic<uint32_t, Order::RELAXED> const* inflight(Shared*);
//         Bytes taken out of the ring and not yet seen complete, or nullptr where an empty
//         ring is an empty channel. See console_ring.h's flush().
//     static uint32_t tx_lost(Shared const*);
//         TX loss the transport counts in a field of its own, added into the wire
//         tx_dropped. 0 where there is none.
// The Shared block it describes must carry `tx`, `rx`, `stats`, `mode`, `tx_buf` and
// `rx_buf`.

#ifndef KICKOS_SYS_CONSOLE_SERVICE_H
#define KICKOS_SYS_CONSOLE_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/atomic.h>
#include <kickos/sys/byte_ring.h>
#include <kickos/sys/bytes.h> // mem_copy, mem_zero
#include <kickos/sys/console_ring.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos::console
{

// The request endpoint the service thread receives on. Every buffered console driver's
// two-thread spawn puts it at this index, and a descriptor granting the right caps in the
// wrong ORDER stalls silently.
enum
{
    KOS_CONSOLE_CAP_EP = KOS_SPAWN_DELEGATED_CAP0
};

// Returns kos_reply's result: a reply can fail on a dead cap, and a caller that has gone is
// the one thing this arm cannot see from its own state.
inline int reply_status(kos_cap_t reply_cap, int32_t status, uint16_t len)
{
    struct kos_uart_rsp rsp;
    rsp.status = status;
    rsp.len = len;
    rsp.rsv = 0;
    return kos_reply(reply_cap, &rsp, sizeof(rsp));
}

// Lay out the shared block. Not thread-safe: call it before either thread exists.
template <typename Transport, typename Shared>
void shared_init(Shared* s)
{
    mem_zero(s, sizeof(*s));
    kos_byte_ring_init(&s->tx, s->tx_buf, static_cast<uint32_t>(sizeof(s->tx_buf)));
    kos_byte_ring_init(&s->rx, s->rx_buf, static_cast<uint32_t>(sizeof(s->rx_buf)));
    if (Transport::MODE_REQUIRED != 0u)
    {
        s->mode = Transport::MODE_REQUIRED;
    }
}

// Parse + run one request frame; the reply is this function's, on every path.
//
// `mode` is null for a service with no unframed console arm, which is what makes
// KOS_UART_SET_MODE refuse there instead of storing a mode nothing reads.
template <typename Transport, typename Shared>
int serve_one(Shared* sh, Atomic<uint32_t, Order::RELAXED>* mode, uint8_t const* msg, size_t n,
              kos_cap_t reply_cap)
{
    if (reply_cap == KOS_CAP_NONE)
    {
        return 0; // a plain send, not a call: nothing to reply to, and nothing to do
    }
    if (n < sizeof(struct kos_uart_req))
    {
        return reply_status(reply_cap, -KOS_EINVAL, 0);
    }
    struct kos_uart_req req;
    mem_copy(&req, msg, sizeof(req));
    uint8_t const* payload = msg + sizeof(req);
    size_t const payload_len = n - sizeof(req);

    switch (req.op)
    {
    case KOS_UART_WRITE:
    {
        if (req.len > payload_len)
        {
            return reply_status(reply_cap, -KOS_EINVAL, 0); // framing claims more than it carried
        }
        // A short accept, zero included, is NOT an error: the client sees `len < req.len`
        // and retries.
        uint32_t const took = tx_write(&sh->tx, &sh->stats, payload, req.len);
        return reply_status(reply_cap, 0, static_cast<uint16_t>(took));
    }
    case KOS_UART_READ:
    {
        if ((req.flags & KOS_UART_F_BLOCK) != 0)
        {
            // Refused rather than returning 0 bytes, so "unsupported" cannot read as
            // "no data".
            return reply_status(reply_cap, -KOS_ENOSYS, 0);
        }
        uint8_t out[KOS_EP_MSG_MAX];
        uint32_t want = req.len;
        if (want > KOS_EP_MSG_MAX - sizeof(struct kos_uart_rsp))
        {
            want = KOS_EP_MSG_MAX - sizeof(struct kos_uart_rsp);
        }
        struct kos_uart_rsp rsp;
        rsp.status = 0;
        rsp.rsv = 0;
        uint32_t const got = kos_byte_ring_pop(&sh->rx, out + sizeof(rsp), want);
        rsp.len = static_cast<uint16_t>(got);
        mem_copy(out, &rsp, sizeof(rsp));
        return kos_reply(reply_cap, out, sizeof(rsp) + got);
    }
    case KOS_UART_STATS:
    {
        uint8_t out[sizeof(struct kos_uart_rsp) + sizeof(struct kos_uart_stats)];
        struct kos_uart_rsp rsp;
        rsp.status = 0;
        rsp.len = static_cast<uint16_t>(sizeof(struct kos_uart_stats));
        rsp.rsv = 0;
        mem_copy(out, &rsp, sizeof(rsp));
        stats_pack(out + sizeof(rsp), &sh->stats, Transport::tx_lost(sh));
        return kos_reply(reply_cap, out, sizeof(out));
    }
    case KOS_UART_SET_MODE:
    {
        return reply_status(reply_cap, mode_apply(mode, req.flags, Transport::MODE_REQUIRED), 0);
    }
    case KOS_UART_CONFIGURE:
    {
        // The device belongs to the IRQ thread, and on a UART the baud divisor is unwritable
        // while TE/RE are set, so a queued CONFIGURE would reprogram mid-frame or lie.
        return reply_status(reply_cap, -KOS_ENOSYS, 0);
    }
    default:
    {
        return reply_status(reply_cap, -KOS_EINVAL, 0);
    }
    }
}

// Recv/dispatch loop with the CONSOLE arm: this endpoint carries TWO protocols, a kos_call
// being a kos_uart_req frame and a plain send raw console bytes, so the recv must be
// info-bearing to tell them apart. Returns only when the endpoint dies, which is the
// respawn signal.
template <typename Transport, typename Shared>
void console_serve_loop(Shared* sh)
{
    uint8_t msg[KOS_EP_MSG_MAX];
    while (true)
    {
        // reply_cap SEATED: the kernel writes it only where the copy out succeeds, and a
        // ZEROED one is stdout's reserved index rather than the empty capability.
        struct kos_recv_info info;
        info.reply_cap = KOS_CAP_NONE;
        int32_t const n = kos_recv(KOS_CONSOLE_CAP_EP, msg, sizeof(msg), &info);
        if (n < 0)
        {
            break; // endpoint dead (EPIPE) or a bad cap: let the bring-up respawn us
        }
        if (info.reply_cap != KOS_CAP_NONE)
        {
            (void)serve_one<Transport>(sh, &sh->mode, msg, static_cast<size_t>(n),
                                       info.reply_cap);
            continue;
        }
        if (n == 0)
        {
            (void)flush(&sh->tx, Transport::inflight(sh)); // zero-length plain send == flush
            continue;
        }
        // Under this endpoint's seated mode: blocking waits for ring room rather than
        // splicing the tail, a plain send having no reply to report a short accept in.
        uint32_t const took =
            write_console(&sh->tx, &sh->stats, msg, static_cast<uint32_t>(n), sh->mode);
        // Refused at the ring, so these bytes never entered tx_bytes; disjoint from any
        // in-flight loss the transport counts separately.
        kos_counter_increment(&sh->stats.tx_dropped, static_cast<uint32_t>(n) - took);
    }
}

}

#endif
