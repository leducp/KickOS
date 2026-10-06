// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The DEVICE-TOUCHING bodies of the UART services. Every function here calls the raw
// UART class, whose implementation the LINK chooses, so folding them into uart_service.cc
// would make the ring side undefined on a board with no UART backend. The contract for each
// is stated at its declaration in <kickos/sys/uart_service.h>.

#include <kickos/sys/uart_service.h>

namespace kickos::uart
{

void irq_pass(struct kos_uart* dev, Shared* sh)
{
    uint8_t seg[KOS_UART_IRQ_SEG];

    uint32_t const got = kos_uart_read(dev, seg, KOS_UART_IRQ_SEG);
    uint32_t const kept = kos_byte_ring_push(&sh->rx, seg, got);
    kos_counter_increment(&sh->stats.rx_dropped, got - kept);

    // kos_uart_write disarms the TX source on a call it accepted whole, so a drain that
    // stopped after a full segment would leave the device disarmed with the ring still
    // loaded and nothing to wake it. The have == 0 call is what disarms.
    //
    // Peek then drop, never pop then write: the device may take fewer bytes than it was
    // offered and a popped byte it refused has nowhere to go back to.
    while (true)
    {
        uint32_t const have = kos_byte_ring_peek(&sh->tx, seg, KOS_UART_IRQ_SEG);
        uint32_t const took = kos_uart_write(dev, seg, have);
        kos_byte_ring_drop(&sh->tx, took);
        if (took < have)
        {
            break;
        }
        if (have < KOS_UART_IRQ_SEG)
        {
            break;
        }
    }
}

void dev_shutdown(struct kos_uart* dev)
{
    (void)kos_uart_flush(dev);
    (void)kos_uart_close(dev);
}

int32_t dev_flush(struct kos_uart* dev)
{
    return kos_uart_flush(dev);
}

namespace
{
    // On expiry the byte is dropped.
    void put_polled(struct kos_uart* dev, uint8_t b)
    {
        for (uint32_t i = 0; i < KOS_UART_TX_SPIN_MAX; i++)
        {
            if (kos_uart_write(dev, &b, 1u) == 1u)
            {
                return;
            }
        }
    }
}

void win_puts(struct kos_uart* dev, char const* s)
{
    for (; *s != '\0'; s++)
    {
        put_polled(dev, static_cast<uint8_t>(*s));
    }
}

void polled_console_loop(struct kos_uart* dev)
{
    uint8_t buf[KOS_EP_MSG_MAX];
    struct kos_reply_recv_opts opts;
    while (true)
    {
        // Info-less: a client kos_call bounces -KOS_ENOTSUP instead of minting a reply cap here.
        kos_reply_recv_opts_init(&opts, console::KOS_CONSOLE_CAP_EP, KOS_RECV_NO_INFO,
                                 KOS_TIMEOUT_NONE);
        int const n =
            kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        if (n < 0)
        {
            break;
        }
        if (n == 0)
        {
            (void)kos_uart_flush(dev);
            continue;
        }
        for (int i = 0; i < n; i++)
        {
            put_polled(dev, buf[i]);
        }
    }
    dev_shutdown(dev);
}

}
