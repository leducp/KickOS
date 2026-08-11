// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A MOCK backend of the raw UART class <kickos/driver/uart.h>: a second definition of the
// five public class symbols, linked in place of a silicon backend.
//
// STATELESS, like the silicon backends: the model lives at kos_uart_config::base, so nothing
// here has file scope and a case can keep its channel on the stack.
//
// HOST-ONLY: an image carrying this could not also link a real UART backend, whose archive
// member would silently lose the name (tests/check_class_backend.sh).

#include <kickos/driver/uart.h>

#include <kickos/sys/errno.h>

#include "uart_mock.h"

#include <stdint.h>

namespace
{
    struct kos_uart_mock* model(struct kos_uart* u)
    {
        return reinterpret_cast<struct kos_uart_mock*>(u->base);
    }
}

extern "C"
{

int32_t kos_uart_open(struct kos_uart* u, struct kos_uart_config const* cfg)
{
    int32_t const bad_cfg = kos_uart_cfg_check(cfg);
    if (bad_cfg != 0)
    {
        return bad_cfg;
    }
    // 8N1 only: any other frame is refused.
    if (cfg->data_bits != 8u or cfg->parity != KOS_UART_PARITY_NONE or cfg->stop_bits != 1u)
    {
        return -KOS_ENOTSUP;
    }
    u->base = cfg->base;
    u->stats = cfg->stats;

    struct kos_uart_mock* m = model(u);
    if (m->rate <= 0)
    {
        return -KOS_ENOSYS; // no divisor to read back: the rate is unknowable, so refuse
    }
    m->opened++;
    // The REQUEST is discarded: what comes back is the modelled divider's rate.
    return m->rate;
}

uint32_t kos_uart_read(struct kos_uart* u, unsigned char* dst, uint32_t n)
{
    struct kos_uart_mock* m = model(u);
    uint32_t got = 0;
    while (got < n and m->rx_pos < m->rx_len)
    {
        dst[got] = m->rx[m->rx_pos];
        m->rx_pos++;
        got++;
    }
    u->stats->rx_bytes += got;
    return got;
}

uint32_t kos_uart_write(struct kos_uart* u, unsigned char const* src, uint32_t n)
{
    struct kos_uart_mock* m = model(u);
    uint32_t i = 0;
    while (i < n and m->tx_room != 0u and m->tx_len < KOS_UART_MOCK_CAP)
    {
        m->tx[m->tx_len] = src[i];
        m->tx_len++;
        m->tx_room--;
        i++;
    }
    // THIS CALL OWNS THE ARM: a return below n armed the source, a return of exactly n
    // disarmed it. Does NOT touch stats.tx_bytes, which the producer that queued the bytes
    // owns.
    m->tx_armed = 0u;
    if (i < n)
    {
        m->tx_armed = 1u;
    }
    return i;
}

int32_t kos_uart_flush(struct kos_uart* u)
{
    struct kos_uart_mock* m = model(u);
    m->flushes++;
    return 0;
}

int32_t kos_uart_close(struct kos_uart* u)
{
    struct kos_uart_mock* m = model(u);
    m->closes++;
    m->tx_armed = 0u;
    return 0; // idempotent: the count is what proves a second call was accepted
}

}
