// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered-UART service over the raw UART class. The contract for each function is stated at
// its declaration in <kickos/sys/uart_service.h>; the request bodies are the shared ones of
// <kickos/sys/console_service.h>, and the bodies that call the raw UART class are in
// uart_service_dev.cc.

#include <kickos/sys/uart_service.h>

#include <stdlib.h>

namespace kickos::uart
{

void shared_init(Shared* s)
{
    console::shared_init<Transport>(s);
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

size_t serve_one(Shared* sh, Atomic<uint32_t, Order::RELAXED>* mode, uint8_t* buf, size_t n)
{
    return console::serve_one<Transport>(sh, mode, buf, n);
}

void serve_loop(Shared* sh)
{
    uint8_t msg[KOS_EP_MSG_MAX];
    struct kos_reply_recv_opts opts;
    kos_reply_recv_opts_init(&opts, KOS_UART_CAP_EP, 0u, KOS_TIMEOUT_NONE);
    // Send this reply when receiving the next request.
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
            // Continue after a client-buffer fault.
            if (serve_transaction_failed(n))
            {
                continue;
            }
            break; // endpoint dead (EPIPE) or a bad cap: let the bring-up respawn us
        }
        // This service requires a call with a reply capability.
        if (opts.info.reply_cap == KOS_CAP_NONE)
        {
            continue;
        }
        reply_len = serve_one(sh, nullptr, msg, static_cast<size_t>(n));
        reply_cap = opts.info.reply_cap;
    }
}

void console_serve_loop(Shared* sh)
{
    console::console_serve_loop<Transport>(sh);
}

void console_thread(void* arg)
{
    console_serve_loop(&static_cast<Ctx*>(arg)->sh);
    exit(0);
}

}
