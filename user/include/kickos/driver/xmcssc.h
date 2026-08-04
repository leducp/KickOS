// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The XMC4800 USIC0-CH1 SSC (SPI) bus SERVICE. The XMC sibling of the K64F
// DSPI0 service (<kickos/driver/k64dspi.h>): a privileged one-time bring-up
// (xmc_spi0_start) enables the U0C1 kernel clock, programs the fixed baud profile +
// SSC-master + internal-loopback input, arms the RX interrupts, creates the request
// ENDPOINT, and spawns the UNPRIVILEGED driver thread that owns the U0C1 register
// window (0x4003_0200, 512 B) and serves the bus wire ABI (<kickos/sys/bus.h>) over
// that endpoint via kos_recv -> transact -> kos_reply.
//
// The client speaks the SAME neutral wrapper the K64F service does
// (<kickos/driver/spi_client.h>): spi_transfer / spi_transact / spi_config over a
// SIGNAL-bearing cap, naming one of KOS_BUS_DEV_MAX device slots per call. Because a
// kos_call caller must be a spawned pool thread (the root/init thread is guarded),
// the client is always a spawned thread that receives the endpoint's SIGNAL cap by
// spawn-time delegation. The bring-up runs in the root/init thread and records the
// endpoint's cap handle so the app, in the same thread with the same cap table, can
// delegate a SIGNAL-narrowed cap to ONE client: the service tracks device slots by the
// caller's own request byte, so several devices behind one client are supported and
// several mutually-untrusting clients are not.
//
// Chip select is the controller's own HARDWARE line (KOS_BUS_CS_HW): the driver
// brackets the whole transaction with MSLS/SELO0, held across the software-paced
// words by PCR.FEM=1 (RM 18.4.5.1; proven by user/apps/xmccshold). SCTR.FLE=63
// hands the frame end to the software TCSR.SOF/EOF markers, so MSLS asserts on the
// first word and releases only after the last: one coherent frame, the
// length-sensitive-target shape.
//
// The data path is INTERNAL LOOP-BACK (DX0 = own transmitter, RM 18.2.3.5): there is
// no external XMC SPI device on the bench, so a byte shifts out DOUT0 and is received
// on DIN0 entirely on-chip (rx == tx). SELO0 is armed but NEVER routed to a port pin
// (the IOCR pin-mux stays privileged and untouched), so the service proves the
// call/reply + bus ABI + CS-hold framing plumbing; the MSLS hold itself is proven
// separately by xmccshold. The channel window is a genuine per-thread capability on
// ARMv7-M PMSA (the MPU covers peripheral space, reprogrammed every switch-in),
// unlike K64F where peripherals are gated coarsely by the AIPS bridge.

#ifndef KICKOS_DRIVER_XMCSSC_H
#define KICKOS_DRIVER_XMCSSC_H

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
    int xmc_spi0_start(struct kos_service_cfg const* cfg);

    // TAKE the USIC0-CH1 SSC service endpoint cap out of the root thread's table
    // (seated by xmc_spi0_start): the caller owns it and closes its own copy.
    // One-shot. A second call, or a service that did not come up, returns KOS_CAP_NONE.
    kos_cap_t xmc_spi0_take_endpoint(void);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_DRIVER_XMCSSC_H
