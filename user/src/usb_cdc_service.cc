// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The service-thread half of the USB CDC-ACM layer. The contract for each function is stated
// at its declaration in <kickos/sys/usb_cdc_service.h>; the Cdc class and irq_loop are
// templated over the controller and stay in that header.

#include <kickos/sys/usb_cdc_service.h>

#include <stdlib.h>

namespace kickos::usb
{

void shared_init(Shared* s)
{
    mem_zero(s, sizeof(*s));
    kos_byte_ring_init(&s->tx, s->tx_buf, KOS_USB_TX_SIZE);
    kos_byte_ring_init(&s->rx, s->rx_buf, KOS_USB_RX_SIZE);
    s->mode = KOS_UART_F_NONBLOCK;
}

uint32_t tx_write(Shared* sh, uint8_t const* p, uint32_t n)
{
    return console::tx_write(&sh->tx, &sh->stats, p, n);
}

uint32_t console_flush(Shared* sh)
{
    return console::flush(&sh->tx, &sh->tx_inflight);
}

uint32_t console_write(Shared* sh, uint8_t const* p, uint32_t n)
{
    return console::write_console(&sh->tx, &sh->stats, p, n,
                                  sh->mode);
}

int reply_status(kos_cap_t reply_cap, int32_t status, uint16_t len)
{
    struct kos_uart_rsp rsp;
    rsp.status = status;
    rsp.len = len;
    rsp.rsv = 0;
    return kos_reply(reply_cap, &rsp, sizeof(rsp));
}

int serve_one(Shared* sh, Atomic<uint32_t, Order::RELAXED>* mode, uint8_t const* msg, size_t n,
              kos_cap_t reply_cap)
{
    if (reply_cap == KOS_CAP_NONE)
    {
        return 0; // a plain send, not a call: nothing to reply to
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
            return reply_status(reply_cap, -KOS_EINVAL, 0);
        }
        // A short accept, zero included, is not an error: the client sees len < req.len and
        // retries.
        uint32_t const took = tx_write(sh, payload, req.len);
        return reply_status(reply_cap, 0, static_cast<uint16_t>(took));
    }
    case KOS_UART_READ:
    {
        if ((req.flags & KOS_UART_F_BLOCK) != 0)
        {
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
        console::stats_pack(out + sizeof(rsp), &sh->stats,
                            sh->tx_lost_link);
        return kos_reply(reply_cap, out, sizeof(out));
    }
    case KOS_UART_SET_MODE:
    {
        return reply_status(reply_cap,
                     console::mode_apply(mode, req.flags, KOS_UART_F_NONBLOCK), 0);
    }
    case KOS_UART_CONFIGURE:
    {
        // A CDC line coding is set by the HOST, over the control endpoint, in the IRQ
        // thread. There is nothing here for a client to program.
        return reply_status(reply_cap, -KOS_ENOSYS, 0);
    }
    default:
    {
        return reply_status(reply_cap, -KOS_EINVAL, 0);
    }
    }
}

void console_serve_loop(Shared* sh)
{
    uint8_t msg[KOS_EP_MSG_MAX];
    while (true)
    {
        struct kos_recv_info info;
        int32_t const n = kos_recv(KOS_USB_CAP_EP, msg, sizeof(msg), &info);
        if (n < 0)
        {
            break; // endpoint dead: let the bring-up respawn us
        }
        if (info.reply_cap != KOS_CAP_NONE)
        {
            (void)serve_one(sh, &sh->mode, msg, static_cast<size_t>(n), info.reply_cap);
            continue;
        }
        if (n == 0)
        {
            (void)console_flush(sh); // zero-length plain send == flush
            continue;
        }
        // Raw console write, under this endpoint's seated NON-BLOCKING mode: a plain send has
        // no reply, so a short accept can only be counted, never reported.
        uint32_t const took = console_write(sh, msg, static_cast<uint32_t>(n));
        // Refused at the ring, so these bytes never entered tx_bytes; disjoint from the
        // in-flight loss drop_in_flight() counts.
        kos_counter_increment(&sh->stats.tx_dropped, static_cast<uint32_t>(n) - took);
    }
    exit(0);
}

}
