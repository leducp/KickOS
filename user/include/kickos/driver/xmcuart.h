// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Polled USIC0 CH0 console driver for XMC4800. The kernel configures clocks,
// pins, and baud rate before handover; the driver writes within its window.
// Do not use libc stdio here: sending to this driver's own endpoint deadlocks.
// Use the USIC window or kos_kconsole_write for diagnostics.
// The MPU grants this window per thread; SCU and pin registers stay privileged.

#ifndef KICKOS_DRIVER_XMCUART_H
#define KICKOS_DRIVER_XMCUART_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/service.h> // kos_service_cfg (the bring-up config)

#ifdef __cplusplus
extern "C"
{
#endif

    // Unprivileged driver entry. arg is the USIC0 CH0 base address, not a pointer
    // to an argument structure. Receive capability is delegated at index 1.
    // Serves byte batches through polled TBUF0 writes. Spawned by
    // xmcuart_console_start or directly by an application.
    void xmcuart_console_driver(void* arg);

    // Call once before starting console clients. Requires AUTH_CONSOLE and
    // AUTH_MEMORY. Creates and publishes an endpoint, spawns the USIC0 CH0 driver,
    // then closes root's WAIT cap so driver death wakes clients with EPIPE.
    // cfg supplies the window and priority; priority must be at least every
    // client's because plain rendezvous has no priority inheritance.
    // Returns 0 or a negative error. Do not start console clients on failure.
    int xmcuart_console_start(struct kos_service_cfg const* cfg);

#ifdef __cplusplus
}
#endif

#endif
