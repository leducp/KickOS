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

int serve_one(Shared* sh, Atomic<uint32_t, Order::RELAXED>* mode, uint8_t const* msg, size_t n,
              kos_cap_t reply_cap)
{
    return console::serve_one<Transport>(sh, mode, msg, n, reply_cap);
}

void serve_loop(Shared* sh)
{
    uint8_t msg[KOS_EP_MSG_MAX];
    while (true)
    {
        // reply_cap SEATED: the kernel writes it only where the copy out succeeds, and a
        // ZEROED one is stdout's reserved index rather than the empty capability.
        struct kos_recv_info info;
        info.reply_cap = KOS_CAP_NONE;
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
    console::console_serve_loop<Transport>(sh);
}

void console_thread(void* arg)
{
    console_serve_loop(&static_cast<Ctx*>(arg)->sh);
    exit(0);
}

}
