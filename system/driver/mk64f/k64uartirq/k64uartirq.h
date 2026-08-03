// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/UART0 buffered IRQ-driven userspace UART driver. An IRQ thread owns the granted
// UART0 register window and drains a TX ring / fills an RX ring from the UART0 status
// interrupt (IRQ 31); a service thread owns the request endpoint and never touches a
// register.
//
// The endpoint serves TWO client shapes on one object, discriminated by whether the
// client sent or called:
//   * a plain kos_send is raw console bytes (what libc stdio over cap 0 produces),
//   * a kos_call is a <kickos/sys/uart.h> frame (WRITE / READ / STATS).
//
// ORDERING: the kernel's own console ring owns IRQ 31 until kos_console_publish runs
// console_tx_deinit, and a claimed line is refused while any handler but the default is
// attached (INVARIANT H2, kernel/irq/irq.cc irq_claim), so the publish must precede the
// claim.
//
// AIPS bridges are not SYSMPU slave ports (RM 3.3.6.2), so the per-thread window grant is
// INERT for the peripheral and the real enabler is the AIPS PACR open. The grant is what
// AUTHORISES that call and what makes the window single-holder, so the service thread
// structurally cannot poke the device. SYSMPU still enforces the memory isolation of the
// shared ring block.

#ifndef KICKOS_DRIVER_MK64F_K64UARTIRQ_H
#define KICKOS_DRIVER_MK64F_K64UARTIRQ_H

#include <kickos/sys/service.h> // kos_service_cfg (the bring-up config)

#ifdef __cplusplus
extern "C"
{
#endif

    // Privileged one-shot bring-up, called ONCE from a service list before any client
    // runs. Needs AUTH_MEMORY, AUTH_CONSOLE and AUTH_IRQ, so it runs as root, not as the
    // driver.
    //
    // `cfg` carries the UART0 window base/size, the baud in cfg->hz (0 = 115200) and the
    // SERVICE thread priority in cfg->prio; the IRQ thread is spawned at cfg->prio + 1,
    // so cfg->prio must be below the priority ceiling and at or above every stdout
    // client (no PI on a rendezvous). cfg->kind must be KOS_SVC_CONSOLE.
    //
    // Returns 0, or -1 on any failure. Every failure path closes the endpoint before
    // reporting, which reclaims the console and is the only reason the tag reaches the
    // wire at all: past the publish, a kernel-console write is a bare DROP.
    int k64uartirq_console_start(struct kos_service_cfg const* cfg);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_DRIVER_MK64F_K64UARTIRQ_H
