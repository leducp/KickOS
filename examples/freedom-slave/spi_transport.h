// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Callable blocking SPI transport over the K64F/DSPI0 unprivileged driver
// (user/apps/k64dspi). The privileged bring-up shim (spi_driver_start) clock-gates
// + PORTD-muxes + configures DSPI0 while halted, opens the DSPI AIPS slot to user
// mode, sets up the software GPIO chip select on PTC4, and spawns the UNPRIVILEGED
// driver thread that owns the DSPI register window + IRQ 26 + the PTC4 CS GPIO.
// Client threads (e.g. a KickCAT slave) then call spi_transfer():
// it copies the caller's buffer into a shared bounce buffer, hands a descriptor to
// the driver thread over a semaphore handshake, and blocks until the driver has run
// the DSPI frames and posted completion. The caller NEVER touches MMIO/grants/IRQs.
//
// The bounce copy is REQUIRED under MPU enforcement: the driver thread cannot read
// a client's private stack (a different domain). The shared descriptor + bounce
// buffer are file-scope state in the app's .appdata window, which the kernel grants
// R|W to every unprivileged thread of the app -- so any client thread reaches them.
//
// KickCAT's AbstractSPI backend (KickCAT lib/slave/driver/src/kickos/SPI.cc)
// declares spi_transfer/spi_enable_cs/spi_disable_cs as extern "C" locally, so it
// links this transport without a KickOS header dependency (the Time-backend seam).

#ifndef KICKOS_APP_K64DSPI_SPI_TRANSPORT_H
#define KICKOS_APP_K64DSPI_SPI_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Privileged bring-up: clock/mux/config DSPI0, open its AIPS slot, create the
    // handshake semaphores, and spawn the unprivileged driver thread. Call ONCE from
    // the privileged app main before any client thread issues a transfer. The
    // loopback flag configures a conservative baud (safe over a SOUT->SIN jumper);
    // pass false for the LAN9252 boot baud/mode (Stage D). Returns 0, or <0 on error.
    int spi_driver_start(int loopback);

    // Blocking full-duplex transfer of `len` bytes. tx==NULL shifts dummy 0x00;
    // rx==NULL discards the received bytes. Returns bytes transferred, or <0.
    // CS follows the most recent spi_enable_cs/spi_disable_cs (software GPIO on PTC4);
    // a transfer outside a bracket self-asserts CS. Serialized: one caller at a time.
    int spi_transfer(void* tx, void* rx, size_t len);

    // Assert / release CS across a header+payload transfer pair (LAN9252 CSR framing).
    // enable drives the PTC4 GPIO low and holds it across the pair; disable drives it
    // high after the last frame drains -- a software CS, no trailing clocked byte.
    void spi_enable_cs(void);
    void spi_disable_cs(void);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_APP_K64DSPI_SPI_TRANSPORT_H
