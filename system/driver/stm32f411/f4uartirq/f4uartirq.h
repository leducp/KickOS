// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 USART2 buffered IRQ-driven userspace UART driver. An IRQ thread owns the granted
// USART2 register window and drains a TX ring / fills an RX ring from the USART2 global
// interrupt (NVIC position 38, RM0383 sec.10.1.3 Table 37); a service thread owns the
// request endpoint and never touches a register.
//
// The endpoint serves TWO client shapes on one object, discriminated by whether the client
// sent or called:
//   * a plain kos_send is raw console bytes (what libc stdio over cap 0 produces),
//   * a kos_call is a <kickos/sys/uart.h> frame (WRITE / READ / STATS).
//
// ORDERING: the kernel's own console ring owns the USART2 vector until kos_console_publish
// runs console_tx_deinit, and a claimed line is refused while any handler but the default is
// attached (kernel/irq/irq.cc irq_claim), so the publish must precede the claim.
//
// ONE VECTOR, LEVEL-SHAPED. Every USART event is ORed into a single request line
// (RM0383 sec.19.4 Figure 191), so a status flag left set holds the NVIC line asserted and
// the line is claimed KOS_IRQ_LEVEL. That is also why the driver, not the service thread,
// must hold the window: only the window holder can clear the flag a rearm would spin on
// (leg L5).
//
// The window grant is the isolation, not a clock gate: no bus-side supervisor-protect and no
// RCC bit sit inside the window, so the grant buys exactly the single-holder property that
// keeps the service thread off the device.

#ifndef KICKOS_DRIVER_STM32F411_F4UARTIRQ_H
#define KICKOS_DRIVER_STM32F411_F4UARTIRQ_H

#include <kickos/sys/service.h> // kos_service_cfg (the bring-up config)

#ifdef __cplusplus
extern "C"
{
#endif

    // Privileged one-shot bring-up: ONCE, before any client runs. Needs AUTH_MEMORY,
    // AUTH_CONSOLE and AUTH_IRQ, so it runs as root, not as the driver.
    //
    // `cfg` carries the USART2 window base/size, the baud in cfg->hz (0 = 115200) and the
    // SERVICE thread priority in cfg->prio; the IRQ thread is spawned at cfg->prio + 1, so
    // cfg->prio must be below the priority ceiling and at or above every stdout client (no
    // PI on a rendezvous). cfg->kind must be KOS_SVC_CONSOLE.
    //
    // A failure path must close the endpoint before reporting: past the publish, a
    // kernel-console write is dropped and the report never reaches the wire.
    int f4uartirq_console_start(struct kos_service_cfg const* cfg);

#ifdef __cplusplus
}
#endif

#endif
