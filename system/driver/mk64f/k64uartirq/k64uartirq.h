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
// attached (kernel/irq/irq.cc irq_claim), so the publish must precede the claim.
//
// The window grant is LOAD-BEARING: it authorises the kos_periph_enable that opens the
// AIPS PACR, and it makes the window single-holder, so the service thread structurally
// cannot poke the device. SYSMPU still enforces the memory isolation of the shared ring
// block.

#ifndef KICKOS_DRIVER_MK64F_K64UARTIRQ_H
#define KICKOS_DRIVER_MK64F_K64UARTIRQ_H

#ifdef __cplusplus
extern "C"
{
#endif

    struct kos_driver_instance;

    // The driver's START. The IRQ thread runs one priority above the task's, which must be
    // below the priority ceiling and at or above every stdout client's (no PI on a
    // rendezvous). Returns 0, or -1 on any failure.
    int k64uartirq_console_start(struct kos_driver_instance* instance);

#ifdef __cplusplus
}
#endif

#endif
