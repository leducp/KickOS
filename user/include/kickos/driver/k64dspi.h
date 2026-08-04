// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K64F/DSPI0 SPI bus SERVICE. A privileged one-time bring-up
// (k64dspi_spi_start) clock-gates + PORTD-muxes + configures DSPI0 while halted,
// opens the DSPI AIPS slot to user mode, sets up the software GPIO chip select on
// PTC4, creates the request ENDPOINT, and spawns the UNPRIVILEGED driver thread
// that owns the DSPI register window + the PTC4 CS GPIO and serves the bus wire ABI
// (<kickos/sys/bus.h>) over that endpoint via kos_recv -> transact -> kos_reply.
//
// The client speaks the neutral wrapper (<kickos/driver/spi_client.h>): spi_transfer /
// spi_transact / spi_config over a SIGNAL-bearing cap on the endpoint, naming one of
// KOS_BUS_DEV_MAX device slots per call. Because a kos_call caller must be a spawned
// pool thread (the root/init thread is guarded), the client is always a spawned thread
// that receives the endpoint's SIGNAL cap by spawn-time delegation. The bring-up runs
// in the root/init thread and records the endpoint's cap handle so the app, in the
// same thread with the same cap table, can delegate a SIGNAL-narrowed cap to ONE
// client: the service tracks device slots by the caller's own request byte, so several
// devices behind one client are supported and several mutually-untrusting clients are not.
//
// Chip select is a SOFTWARE GPIO on PTC4 (Arduino D9 = LAN9252 SCS), NOT hardware
// PCS0: the driver brackets the whole transaction by driving PTC4 low (assert) /
// high (release) around all segments. This fixes the confirmed Stage-D bug: DSPI's
// CONT/PCS model has no zero-clock CS deassert, so releasing hardware PCS0 clocked a
// trailing dummy byte that corrupted length-sensitive LAN9252 mailbox writes.
//
// GPIO access path (K64 RM 3.10.1.1 / 3.3.6.2 / 3.3.7.1 / 4.6): GPIO is a direct
// crossbar slave with NO access protection: not an AIPS-Lite slot (no PACR) and
// not a SYSMPU slave port. So the unprivileged driver reaches GPIOC's data registers
// with NO grant; only the PTC4 pin-mux + direction are set privileged one-time in
// the bring-up. DSPI0 privilege stays AIPS-PACR slot 44 as before.

#ifndef KICKOS_DRIVER_K64DSPI_H
#define KICKOS_DRIVER_K64DSPI_H

#include <kickos/sys/abi.h> // kos_cap_t (the endpoint handle this hands out)

#ifdef __cplusplus
extern "C"
{
#endif

    struct kos_service_cfg;

    // KOS_SVC_SPI service start(): the privileged one-time bring-up + endpoint +
    // unprivileged driver spawn. Reads the controller base/window, target Hz, and
    // CS policy from the service cfg. Returns 0, or a negative -KOS_E*. NO libc
    // stdio (the service-list HARD RULE); diagnostics use kos::print.
    int k64dspi_spi_start(struct kos_service_cfg const* cfg);

    // TAKE the DSPI0 service endpoint cap out of the root thread's table (seated by
    // k64dspi_spi_start): the caller owns it and closes its own copy.
    // One-shot. A second call, or a service that did not come up, returns KOS_CAP_NONE.
    kos_cap_t k64dspi_take_endpoint(void);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_DRIVER_K64DSPI_H
