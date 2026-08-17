// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered-UART service over the raw UART class. The contract for each function is stated at
// its declaration in <kickos/sys/uart_service.h>; the bodies that call the raw UART class are
// in uart_service_dev.cc.

#include <kickos/sys/uart_service.h>

#include <stdlib.h>

namespace kickos::uart
{

void shared_init(Shared* s)
{
    mem_zero(s, sizeof(*s));
    kos_byte_ring_init(&s->tx, s->tx_buf, KOS_UART_TX_SIZE);
    kos_byte_ring_init(&s->rx, s->rx_buf, KOS_UART_RX_SIZE);
}

int ctx_init(Ctx* ctx, struct kos_service_cfg const* cfg, uint32_t fallback_baud)
{
    shared_init(&ctx->sh);
    uint32_t baud = cfg->hz;
    if (baud == 0u)
    {
        baud = fallback_baud;
    }
    ctx->ucfg.base = cfg->mmio_base;
    ctx->ucfg.stats = &ctx->sh.stats;
    ctx->ucfg.baud = baud;
    ctx->ucfg.data_bits = 8;
    ctx->ucfg.parity = KOS_UART_PARITY_NONE;
    ctx->ucfg.stop_bits = 1;
    ctx->ucfg.rsv = 0;
    return 0;
}

uint32_t tx_write(Shared* sh, uint8_t const* p, uint32_t n)
{
    return console::tx_write(&sh->tx, &sh->stats, p, n);
}

uint32_t console_flush(Shared* sh)
{
    return console::flush(&sh->tx, nullptr);
}

uint32_t push_all(Shared* sh, uint8_t const* p, uint32_t n)
{
    return console::push_all(&sh->tx, &sh->stats, p, n);
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
        uint32_t const took = tx_write(sh, payload, req.len);
        // A short accept is NOT an error: the client sees `len < req.len` and retries.
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
        console::stats_pack(out + sizeof(rsp), &sh->stats, 0u);
        return kos_reply(reply_cap, out, sizeof(out));
    }
    case KOS_UART_SET_MODE:
    {
        // Nothing required: a UART drains unconditionally, so blocking is honourable here.
        return reply_status(reply_cap, console::mode_apply(mode, req.flags, 0u), 0);
    }
    case KOS_UART_CONFIGURE:
    {
        // The device belongs to the IRQ thread, and the baud divisor is unwritable while
        // TE/RE are set, so a queued CONFIGURE would reprogram mid-frame or lie.
        return reply_status(reply_cap, -KOS_ENOSYS, 0);
    }
    default:
    {
        return reply_status(reply_cap, -KOS_EINVAL, 0);
    }
    }
}

void serve_loop(Shared* sh)
{
    uint8_t msg[KOS_EP_MSG_MAX];
    while (true)
    {
        struct kos_recv_info info;
        int32_t const n = kos_recv(KOS_UART_CAP_EP, msg, sizeof(msg), &info);
        if (n < 0)
        {
            break; // endpoint dead (EPIPE) or a bad cap: let the bring-up respawn us
        }
        // A failed reply leaves that caller parked; one dead caller is not the service's end.
        (void)serve_one(sh, nullptr, msg, static_cast<size_t>(n), info.reply_cap);
    }
}

void console_serve_loop(Shared* sh)
{
    uint8_t msg[KOS_EP_MSG_MAX];
    while (true)
    {
        struct kos_recv_info info;
        int32_t const n = kos_recv(KOS_UART_CAP_EP, msg, sizeof(msg), &info);
        if (n < 0)
        {
            break; // endpoint dead (EPIPE) or a bad cap: let the bring-up respawn us
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
        // Waits for ring room rather than splicing the tail: a plain send has no reply to
        // report a short accept in.
        uint32_t const took = console_write(sh, msg, static_cast<uint32_t>(n));
        kos_counter_increment(&sh->stats.tx_dropped, static_cast<uint32_t>(n) - took);
    }
}

void console_thread(void* arg)
{
    console_serve_loop(&static_cast<Ctx*>(arg)->sh);
    exit(0);
}

}
