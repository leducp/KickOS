// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Polled UART0 console driver for K64F. The kernel configures clocks, pins,
// and baud rate before handover; the driver writes within its UART0 window.
// Do not use libc stdio here: sending to this driver's own endpoint deadlocks.
// Use the UART window or kos_kconsole_write for diagnostics.
//
// K64F peripheral MMIO is not protected per thread by SYSMPU (RM 3.3.6.2).
// kos_periph_enable opens UART0 through AIPS for all unprivileged threads;
// only the window holder may request this. SYSMPU still isolates memory.

#ifndef KICKOS_DRIVER_K64UART_H
#define KICKOS_DRIVER_K64UART_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/service.h> // kos_service_cfg (the bring-up config)

#ifdef __cplusplus
extern "C"
{
#endif

    // Unprivileged driver entry. arg is the UART0 base address, not a pointer
    // to an argument structure. Receive capability is delegated at index 1.
    // Serves byte batches through polled UART writes. Spawned by
    // k64uart_console_start or directly by an application.
    void k64uart_console_driver(void* arg);

    // Call once before starting console clients. Requires AUTH_CONSOLE and
    // AUTH_MEMORY. Creates and publishes an endpoint, spawns the UART0 driver,
    // then closes root's WAIT cap so driver death wakes clients with EPIPE.
    // cfg supplies the window and priority; priority must be at least every
    // client's because plain rendezvous has no priority inheritance.
    // Returns 0 or a negative error. Do not start console clients on failure.
    int k64uart_console_start(struct kos_service_cfg const* cfg);

#ifdef __cplusplus
}
#endif

#endif
