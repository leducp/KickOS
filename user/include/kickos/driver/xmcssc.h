// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The XMC4800 USIC0-CH1 SSC (SPI) bus SERVICE, the XMC sibling of the K64F DSPI0 service
// (<kickos/driver/k64dspi.h>). A client reaches it through the SPI class
// <kickos/driver/spi.h> with kickos_spi_proxy as its SPI_BACKEND. Device slots are tracked by
// the caller's own request byte, so several devices behind ONE client are supported and
// several mutually-untrusting clients are not.
//
// Chip select is the controller's own HARDWARE line (KOS_BUS_CS_HW), held across the
// software-paced words by PCR.FEM=1 (RM 18.4.5.1; proven by user/apps/xmc4800-relax/xmccshold). SCTR.FLE=63
// hands the frame end to the software TCSR.SOF/EOF markers, so MSLS asserts on the first word
// and releases only after the last.
//
// THE DATA PATH IS INTERNAL LOOP-BACK (DX0 = own transmitter, RM 18.2.3.5): rx == tx entirely
// on-chip, and SELO0 is armed but NEVER routed to a port pin, the IOCR pin-mux staying
// privileged and untouched. So nothing here witnesses the MSLS hold itself; xmccshold does.

#ifndef KICKOS_DRIVER_XMCSSC_H
#define KICKOS_DRIVER_XMCSSC_H

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
    int xmc_spi0_start(struct kos_service_cfg const* cfg);

    // TAKE the USIC0-CH1 SSC service endpoint cap out of the root thread's table: the caller
    // owns it and closes its own copy. ONE-SHOT; a second call, or a service that did not come
    // up, returns KOS_CAP_NONE.
    kos_cap_t xmc_spi0_take_endpoint(void);

#ifdef __cplusplus
}
#endif

#endif
