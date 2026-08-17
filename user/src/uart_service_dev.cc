// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The DEVICE-TOUCHING bodies of the buffered-UART service. Every function here calls the raw
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

void win_puts(struct kos_uart* dev, char const* s)
{
    for (; *s != '\0'; s++)
    {
        uint8_t const b = static_cast<uint8_t>(*s);
        for (uint32_t i = 0; i < KOS_UART_TX_SPIN_MAX; i++)
        {
            if (kos_uart_write(dev, &b, 1u) == 1u)
            {
                break;
            }
        }
    }
}

}
