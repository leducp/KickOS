// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K64F/DSPI0 SPI bus SERVICE. A client reaches it through the SPI class
// <kickos/driver/spi.h> with kickos_spi_proxy as its SPI_BACKEND. Device slots are tracked by
// the caller's own request byte, so several devices behind ONE client are supported and
// several mutually-untrusting clients are not.
//
// Chip select is a SOFTWARE GPIO on PTC4 (Arduino D9 = LAN9252 SCS), NOT hardware PCS0: DSPI's
// CONT/PCS model has no zero-clock CS deassert, so releasing hardware PCS0 clocked a trailing
// dummy byte that corrupted length-sensitive LAN9252 mailbox writes.
//
// GPIO is a direct crossbar slave with NO access protection (K64 RM 3.10.1.1 / 3.3.6.2 /
// 3.3.7.1 / 4.6): not an AIPS-Lite slot, not a SYSMPU slave port. So the unprivileged driver
// reaches GPIOC's data registers with no grant; only the PTC4 pin-mux and direction are set
// privileged one-time in the bring-up. DSPI0 privilege stays AIPS-PACR slot 44.

#ifndef KICKOS_DRIVER_K64DSPI_H
#define KICKOS_DRIVER_K64DSPI_H

#include <kickos/sys/abi.h> // kos_cap_t (the endpoint handle this hands out)

#ifdef __cplusplus
extern "C"
{
#endif

    struct kos_service_cfg;

    // KOS_SVC_SPI service start(): the privileged one-time bring-up, the endpoint, and the
    // unprivileged driver spawn. Reads the controller base/window, target Hz and CS policy
    // from the service cfg. Returns 0, or a negative -KOS_E*. NO libc stdio (the service-list
    // HARD RULE); diagnostics use kos::print.
    int k64dspi_spi_start(struct kos_service_cfg const* cfg);

    // TAKE the DSPI0 service endpoint cap out of the root thread's table: the caller owns it
    // and closes its own copy. ONE-SHOT; a second call, or a service that did not come up,
    // returns KOS_CAP_NONE.
    kos_cap_t k64dspi_take_endpoint(void);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_DRIVER_K64DSPI_H
