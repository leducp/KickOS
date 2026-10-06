// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 IRQ-driven buffered UART console driver on USIC0 CH0 (U0C0), taken at the
// console handover (kos_console_publish).
//
// TX-ONLY: kos_uart_read() has no receive path, so KOS_UART_READ always returns 0 bytes.
// Adding RX needs CCR |= RIEN|AIEN plus a PSCR W1C of the receive flags before every
// re-arm, or the level re-asserts and storms SR1.
//
// This task's stdout is the kernel console, which the wire does not carry while this driver
// owns the UART.
//
// The driver cannot CHANGE the baud rate: FDR and BRG are Write = PV (RM Table 18-20) and
// the kos_periph_reg_write allowlist carries entries for them on the sibling channel U0C1
// only, so a U0C0 rate request is refused -KOS_EPERM. Both registers are
// unprivileged-readable, so the achieved rate is read back rather than echoed. CCR is the
// driver's ONLY privileged write.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source.

#ifndef KICKOS_DRIVER_XMCUARTIRQ_H
#define KICKOS_DRIVER_XMCUARTIRQ_H

#ifdef __cplusplus
extern "C"
{
#endif

    struct kos_driver_instance;

    // The driver's START. The IRQ thread runs one priority above the task's, which must be
    // below the priority ceiling and at or above every stdout client's (no PI on a
    // rendezvous). Returns 0, or -1 on any failure.
    int xmcuartirq_console_start(struct kos_driver_instance* instance);

#ifdef __cplusplus
}
#endif

#endif
