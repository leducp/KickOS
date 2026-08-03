// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 IRQ-driven buffered UART console driver on USIC0 CH0 (U0C0), taken at the
// console handover (kos_console_publish), after which the kernel's own ring has released
// the channel and irq_detach'd NVIC 84. An UNPRIVILEGED IRQ thread owns the granted U0C0
// window and drains the shared TX ring on the USIC transmit-buffer interrupt; a second
// UNPRIVILEGED service thread owns the console endpoint and only ever touches the ring.
//
// TX-ONLY BY CONSTRUCTION: service_irq() has no receive path, so KOS_UART_READ always
// returns 0 bytes. Adding RX needs CCR |= RIEN|AIEN (already inside the granted mask) plus
// a PSCR W1C of the receive flags before every re-arm, or the level re-asserts and storms
// SR1 (validated on silicon by xmcssc on the same USIC IP), and a decision on whether an
// ASC single-byte frame raises RIF or AIF.
//
// Neither thread may use libc stdio: printf/puts route to cap 0, which the publish seated
// on THIS driver's own endpoint, so a self-send would park the sole receiver forever
// (design D7). Diagnostics go through kos_print, which bypasses the endpoint.
//
// The driver does NOT touch clock, pins or baud. FDR and BRG are Write = PV and carry no
// U0C0 allowlist entry, so a store to them is discarded at the bus and the kernel's
// kickos_xmc_usic_init() values stand. CCR is the driver's ONE privileged write.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source.

#ifndef KICKOS_DRIVER_XMCUARTIRQ_H
#define KICKOS_DRIVER_XMCUARTIRQ_H

#include <kickos/sys/service.h> // kos_service_cfg (the bring-up config)

#ifdef __cplusplus
extern "C"
{
#endif

    // Privileged-or-AUTH-bearing one-shot console-handover bring-up. Call ONCE, before
    // spawning any app that should print through the driver.
    //
    // `cfg` must be a KOS_SVC_CONSOLE entry carrying the U0C0 window base/size and the
    // driver priority as data; cfg->prio must be >= every stdout client's priority (D9:
    // no priority inheritance on the console rendezvous), and cfg->prio + 1 must be a
    // valid priority because the IRQ thread sits strictly above the service thread.
    //
    // The caller needs KOS_AUTH_MEMORY (arena + window grant), KOS_AUTH_CONSOLE (the
    // publish) and KOS_AUTH_IRQ (the line mint). Returns 0, or < 0 on any failure; on
    // failure the console has already come back, and the caller MUST NOT spawn
    // console-dependent apps (S6: publish + spawn are inseparable).
    int xmcuartirq_console_start(struct kos_service_cfg const* cfg);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_DRIVER_XMCUARTIRQ_H
