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
// Neither thread may use libc stdio: printf/puts route to cap 0, which the publish seated
// on THIS driver's own endpoint, so a self-send would park the sole receiver forever
// (design D7). Diagnostics go through kos_print, which bypasses the endpoint.
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

#include <kickos/sys/service.h> // kos_service_cfg

#ifdef __cplusplus
extern "C"
{
#endif

    // Call ONCE, before spawning any app that should print through the driver.
    //
    // `cfg` must be a KOS_SVC_CONSOLE entry carrying the U0C0 window base/size and the
    // driver priority as data. cfg->prio must be >= every stdout client's priority (D9: no
    // priority inheritance on the console rendezvous), and cfg->prio + 1 must be a valid
    // priority. cfg->hz must be 0: any other value is a rate request this channel cannot
    // honour, and the bring-up fails with the device left to the kernel.
    //
    // The caller needs KOS_AUTH_MEMORY, KOS_AUTH_CONSOLE and KOS_AUTH_IRQ. Returns 0, or
    // < 0 with the console already back, in which case the caller MUST NOT spawn
    // console-dependent apps: publish and spawn are inseparable.
    int xmcuartirq_console_start(struct kos_service_cfg const* cfg);

#ifdef __cplusplus
}
#endif

#endif
