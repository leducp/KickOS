// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M/SCI6 buffered IRQ-driven userspace UART driver. An IRQ thread owns the granted
// SCI6 register window and drains a TX ring / fills an RX ring; a service thread owns the
// request endpoint and never touches a register.
//
// The SCI6 sources this driver claims, and the class it cannot:
//   TXI6 (vector 87, EDGE): the IRQ thread's own line, pacing the TX drain.
//   RXI6 (vector 86, EDGE): a third thread waits on it and rings the IRQ thread's
//     doorbell. One thread cannot wait on two lines, and a DEV window has exactly one
//     holder (kernel/domain/domain.cc). The relay may rearm before the byte has been read
//     only because the source is EDGE: it does not re-assert on its own.
//   TEI6 / ERI6 (GROUPBL0 sources 268 / 269, LEVEL): NOT claimed. A level source stays
//     asserted until the window holder clears the peripheral flag, so a relay would rearm
//     into a still-asserted source and spin. TEND and ORER/FER/PER are read and cleared
//     in every service_irq pass instead, so error recovery waits for the next event.
//
// ORDERING: the kernel's own console ring owns vector 87 until kos_console_publish runs
// console_tx_deinit, and a claimed line is refused while any handler but the default is
// attached (INVARIANT H2), so the publish must precede the claim. The INTB slot for 87 is
// fixed in flash and routes to a claimed line only once that ring is disarmed
// (arch/rx/rxv3 kickos_rx_console_txi_isr).
//
// The RX MPU checks every user-mode access over the whole address space, SFR aperture
// included, with no carve-out (UM sec.17.1 and Table 17.1), so the window grant is real
// enforcement here. The 16-byte SCI6 window is the RX MPU minimum, so it also exposes
// SNFR..TDRL at +0x08..+0x0F; SCI5 ends below the page and SCI7 sits in a different
// aperture (0x000D00E0).

#ifndef KICKOS_DRIVER_RX72M_RXSCI_H
#define KICKOS_DRIVER_RX72M_RXSCI_H

#include <kickos/sys/service.h> // kos_service_cfg (the bring-up config)

#ifdef __cplusplus
extern "C"
{
#endif

    // Privileged one-shot bring-up, called ONCE from a service list before any client
    // runs. Needs AUTH_MEMORY, AUTH_CONSOLE and AUTH_IRQ, so it runs as root, never as
    // the driver.
    //
    // `cfg` must be KOS_SVC_CONSOLE with mmio_base either 0 or the SCI6 base. cfg->prio
    // is the SERVICE thread priority and must sit at or above every stdout client: there
    // is no priority inheritance on a rendezvous. The IRQ thread and the RX relay run at
    // cfg->prio + 1. cfg->hz is ignored; the baud divisor is not reprogrammed on a live
    // channel, so the driver runs at the console's 115200 and reports that.
    //
    // Returns 0, or -1 on any failure. A failure before the handover tail leaves the
    // console reclaimed by the kernel.
    int rxsci_console_start(struct kos_service_cfg const* cfg);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_DRIVER_RX72M_RXSCI_H
