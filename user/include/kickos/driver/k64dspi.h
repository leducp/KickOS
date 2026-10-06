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

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    struct kos_driver_instance;

    // The SPI bus driver's START: the bus pins muxed, then the bring-up. Returns 0, or a
    // negative -KOS_E*.
    int k64dspi_spi_start(struct kos_driver_instance* instance);

    // Mux the pins the board file wires to the DSPI at `base`: its signals at the selector the
    // chip file names, its chip selects as GPIO. The caller holds pinmux authority. Returns 0,
    // -KOS_EINVAL where no board bus is over `base`, or kos_pinmux_set's refusal.
    int32_t k64dspi_bus_mux(uintptr_t base);

#ifdef __cplusplus
}
#endif

#endif
