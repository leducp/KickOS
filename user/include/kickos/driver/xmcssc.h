// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The XMC4800 USIC0-CH1 SSC (SPI) bus SERVICE, the XMC sibling of the K64F DSPI0 service
// (<kickos/driver/k64dspi.h>). A client reaches it through the SPI class
// <kickos/driver/spi.h> with kickos_spi_proxy linked as its backend. Device slots are tracked by
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

#ifdef __cplusplus
extern "C"
{
#endif

    struct kos_driver_instance;

    // The SPI bus driver's START. Returns 0, or a negative -KOS_E*.
    int xmc_spi0_start(struct kos_driver_instance* instance);

#ifdef __cplusplus
}
#endif

#endif
