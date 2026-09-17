// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The REQUEST side of a published console: the <kickos/sys/uart.h> op dispatch and the
// two-protocol recv loop. The ring side is <kickos/sys/console_ring.h>, which states the
// rules, the budgets and the CRLF posture.
//
// A Transport is a plain struct of constants and static functions:
//     static constexpr uint32_t MODE_REQUIRED;
//         Mode bits the transport cannot clear, and the mode shared_init seats.
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
#include <kickos/sys/serve.h>
#include <kickos/sys/uart.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos::console
{

// The request endpoint the service thread receives on. A descriptor granting the right caps
// in the wrong ORDER stalls silently.
enum
{
    KOS_CONSOLE_CAP_EP = KOS_SPAWN_DELEGATED_CAP0
};

// Build a status-only response OVER the request, and answer its length. Nothing here
// replies: the loop carries the answer out on the call that takes the next request.
inline size_t reply_status(uint8_t* buf, int32_t status, uint16_t len)
{
    struct kos_uart_rsp rsp;
    rsp.status = status;
    rsp.len = len;
    rsp.rsv = 0;
    mem_copy(buf, &rsp, sizeof(rsp));
    return sizeof(rsp);
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

// Parse + run one request frame IN PLACE: the response is built over the request in `buf` and
// its length answered, on every path. A path answering 0 would leak the reply capability and
// park the client forever. Every arm consumes what it needs of the request before it writes,
// so no arm overwrites bytes it still has to read.
//
// `mode` is null for a service with no unframed console arm, where KOS_UART_SET_MODE refuses.
template <typename Transport, typename Shared>
size_t serve_one(Shared* sh, Atomic<uint32_t, Order::RELAXED>* mode, uint8_t* buf, size_t n)
{
    if (n < sizeof(struct kos_uart_req))
    {
        return reply_status(buf, -KOS_EINVAL, 0);
    }
    struct kos_uart_req req;
    mem_copy(&req, buf, sizeof(req));
    uint8_t const* payload = buf + sizeof(req);
    size_t const payload_len = n - sizeof(req);

    switch (req.op)
    {
    case KOS_UART_WRITE:
    {
        if (req.len > payload_len)
        {
            return reply_status(buf, -KOS_EINVAL, 0);
        }
        // A short accept, zero included, is NOT an error: the client sees `len < req.len`
        // and retries.
        uint32_t const took = tx_write(&sh->tx, &sh->stats, payload, req.len);
        return reply_status(buf, 0, static_cast<uint16_t>(took));
    }
    case KOS_UART_READ:
    {
        if ((req.flags & KOS_UART_F_BLOCK) != 0)
        {
            return reply_status(buf, -KOS_ENOSYS, 0);
        }
        uint32_t want = req.len;
        if (want > KOS_EP_MSG_MAX - sizeof(struct kos_uart_rsp))
        {
            want = KOS_EP_MSG_MAX - sizeof(struct kos_uart_rsp);
        }
        struct kos_uart_rsp rsp;
        rsp.status = 0;
        rsp.rsv = 0;
        // The ring writes FRESH bytes over the request, which this arm has finished with.
        uint32_t const got = kos_byte_ring_pop(&sh->rx, buf + sizeof(rsp), want);
        rsp.len = static_cast<uint16_t>(got);
        mem_copy(buf, &rsp, sizeof(rsp));
        return sizeof(rsp) + got;
    }
    case KOS_UART_STATS:
    {
        struct kos_uart_rsp rsp;
        rsp.status = 0;
        rsp.len = static_cast<uint16_t>(sizeof(struct kos_uart_stats));
        rsp.rsv = 0;
        mem_copy(buf, &rsp, sizeof(rsp));
        stats_pack(buf + sizeof(rsp), &sh->stats, Transport::tx_lost(sh));
        return sizeof(rsp) + sizeof(struct kos_uart_stats);
    }
    case KOS_UART_SET_MODE:
    {
        return reply_status(buf, mode_apply(mode, req.flags, Transport::MODE_REQUIRED), 0);
    }
    case KOS_UART_CONFIGURE:
    {
        // The device belongs to the IRQ thread, and the baud divisor is unwritable while
        // TE/RE are set.
        return reply_status(buf, -KOS_ENOSYS, 0);
    }
    default:
    {
        return reply_status(buf, -KOS_EINVAL, 0);
    }
    }
}

// Recv/dispatch loop with the CONSOLE arm. This endpoint carries TWO protocols: a kos_call is
// a kos_uart_req frame, a plain send is raw console bytes, and only an info-bearing recv tells
// them apart. Returns only when the endpoint dies, which is the respawn signal.
template <typename Transport, typename Shared>
void console_serve_loop(Shared* sh)
{
    uint8_t msg[KOS_EP_MSG_MAX];
    struct kos_reply_recv_opts opts;
    kos_reply_recv_opts_init(&opts, KOS_CONSOLE_CAP_EP, 0u, KOS_TIMEOUT_NONE);
    // Carried one pass forward: the answer to request k rides the call that takes k+1.
    kos_cap_t reply_cap = KOS_CAP_NONE;
    size_t reply_len = 0;
    while (true)
    {
        // reply_cap SEATED: the kernel writes it only where the copy out succeeds, and a
        // ZEROED one is stdout's reserved index rather than the empty capability.
        opts.info.reply_cap = KOS_CAP_NONE;
        int32_t const n = kos_reply_recv(reply_cap, msg,
                                         kos_call_lens_pack(reply_len, sizeof(msg)), &opts);
        reply_cap = KOS_CAP_NONE;
        reply_len = 0;
        if (n < 0)
        {
            // One client may not take the console away from the others: a refusal that names
            // the far end of a copy ends that transaction and nothing else. The reply
            // capability is consumed on every exit of the call, so there is nothing to drop.
            if (serve_transaction_failed(n))
            {
                continue;
            }
            break;
        }
        if (opts.info.reply_cap != KOS_CAP_NONE)
        {
            reply_len = serve_one<Transport>(sh, &sh->mode, msg, static_cast<size_t>(n));
            reply_cap = opts.info.reply_cap;
            continue;
        }
        if (n == 0)
        {
            (void)flush(&sh->tx, Transport::inflight(sh)); // zero-length plain send == flush
            continue;
        }
        uint32_t const took =
            write_console(&sh->tx, &sh->stats, msg, static_cast<uint32_t>(n), sh->mode);
        // Disjoint from the in-flight loss the transport counts: these bytes never entered
        // tx_bytes.
        kos_counter_increment(&sh->stats.tx_dropped, static_cast<uint32_t>(n) - took);
    }
}

}

#endif
