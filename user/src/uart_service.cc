// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered-UART service over the raw UART class. The contract for each function is stated at
// its declaration in <kickos/sys/uart_service.h>; the request bodies are the shared ones of
// <kickos/sys/console_service.h>, and the bodies that call the raw UART class are in
// uart_service_dev.cc.

#include <kickos/sys/uart_service.h>

namespace kickos::uart
{

void shared_init(Shared* s)
{
    console::shared_init<Transport>(s);
}

int ctx_init(Ctx* ctx, struct kos_driver_instance const* in, uint32_t fallback_baud)
{
    shared_init(&ctx->sh);
    ctx->ucfg.base = in->mmio_base;
    ctx->ucfg.stats = &ctx->sh.stats;
    ctx->ucfg.baud = fallback_baud;
    ctx->ucfg.data_bits = 8;
    ctx->ucfg.parity = KOS_UART_PARITY_NONE;
    ctx->ucfg.stop_bits = 1;
    ctx->ucfg.rsv = 0;
    ctx->ucfg.line_index = 0;
    if (in->line_count != 0u)
    {
        ctx->ucfg.line_index = in->lines[0].index;
    }
    return 0;
}

int32_t flush(Shared* sh)
{
    if (console::flush(&sh->tx, nullptr) != 0u)
    {
        return -KOS_EBUSY;
    }
    uint32_t const req = sh->flush_req + 1u;
    sh->flush_req = req;
    (void)kos_notify(KOS_UART_CAP_DOORBELL);
    for (uint32_t i = 0; i < console::KOS_CONSOLE_FLUSH_MAX; i++)
    {
        if (sh->flush_ack == req)
        {
            return 0;
        }
        kos_sleep_ns(console::KOS_CONSOLE_FLUSH_SLEEP_NS);
    }
    if (sh->flush_ack == req)
    {
        return 0;
    }
    return -KOS_EBUSY;
}

void console_thread(void* arg)
{
    (void)console::console_serve_loop<Transport>(&static_cast<Ctx*>(arg)->sh);
    // Not the entry thread: its exit would end it alone and leave the task alive.
    driver::trap();
}

}
