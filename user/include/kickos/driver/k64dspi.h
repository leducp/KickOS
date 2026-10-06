// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K64F/DSPI0 SPI bus SERVICE. A client reaches it through the SPI class
// <kickos/driver/spi.h> with kickos_spi_proxy linked as its backend. Device slots are tracked
// by the caller's own request byte, so several devices behind ONE client are supported and
// several mutually-untrusting clients are not.
//
// Chip select is a SOFTWARE GPIO, one of the chip selects the board file wires to the bus, NOT
// hardware PCS0: DSPI's CONT/PCS model has no zero-clock CS deassert, so releasing hardware PCS0
// clocked a trailing dummy byte that corrupted length-sensitive LAN9252 mailbox writes.

#ifndef KICKOS_DRIVER_K64DSPI_H
#define KICKOS_DRIVER_K64DSPI_H

#include <kickos/sys/abi.h> // kos_cap_t (the endpoint handle this hands out)

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    struct kos_service_cfg;

    // KOS_SVC_SPI service start(): the bus pins muxed, the endpoint, and the unprivileged driver
    // spawn. Reads the controller base/window from the service cfg. Returns 0, or a negative
    // -KOS_E*. NO libc stdio (the service-list HARD RULE).
    int k64dspi_spi_start(struct kos_service_cfg const* cfg);

    // Mux the pins the board file wires to the DSPI at `base`: its signals at the selector the
    // chip file names, its chip selects as GPIO. The caller holds pinmux authority. Returns 0,
    // -KOS_EINVAL where no board bus is over `base`, or kos_pinmux_set's refusal.
    int32_t k64dspi_bus_mux(uintptr_t base);

    // TAKE the DSPI0 service endpoint cap out of the root thread's table: the caller owns it
    // and closes its own copy. ONE-SHOT; a second call, or a service that did not come up,
    // returns KOS_CAP_NONE.
    kos_cap_t k64dspi_take_endpoint(void);

#ifdef __cplusplus
}
#endif

#endif
