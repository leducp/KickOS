// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M/SCI6 buffered IRQ-driven userspace UART driver.
//
// TXI6 (vector 87) and RXI6 (vector 86) are EDGE. The RX relay may rearm before the byte
// has been read only because of that. TEI6 / ERI6 (GROUPBL0 268 / 269) are LEVEL and are
// NOT claimed: a relay cannot clear the peripheral flag, so it would rearm into a
// still-asserted source and spin. Their latches are cleared inside kos_uart_flush and
// kos_uart_read instead, so error recovery waits for the next event.
//
// kos_console_publish MUST precede the claim: the kernel console ring holds vector 87
// until then, and a claim is refused while any handler but the default is attached.
//
// The RX MPU checks every user-mode access over the whole address space, SFR aperture
// included, with no carve-out (UM sec.17.1 and Table 17.1). 16 bytes is the MPU minimum,
// so the SCI6 window also exposes SNFR..TDRL at +0x08..+0x0F.

#ifndef KICKOS_DRIVER_RX72M_RXSCI_H
#define KICKOS_DRIVER_RX72M_RXSCI_H

#ifdef __cplusplus
extern "C"
{
#endif

    struct kos_driver_instance;

    // The driver's START. The IRQ thread and the RX relay run one priority above the task's,
    // which must sit at or above every stdout client's: a rendezvous has no priority
    // inheritance. Returns 0, or -1 on any failure.
    int rxsci_console_start(struct kos_driver_instance* instance);

#ifdef __cplusplus
}
#endif

#endif
