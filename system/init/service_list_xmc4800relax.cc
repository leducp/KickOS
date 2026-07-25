// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// xmc4800-relax service-list provider: the board's kickos_board_services, brought up
// in array order by the default init BEFORE the app's main. Two entries:
//   [0] KOS_SVC_CONSOLE -> xmcuart_console_start (USIC0 CH0 handover; console FIRST
//       so the app's stdout reaches the wire before anything else runs).
//   [1] KOS_SVC_SPI     -> xmc_spi0_start        (USIC0 CH1 SSC bus service on an endpoint).
// Per-instance config travels as DATA (kos_service_cfg), never as literals in a
// driver TU. Selected by KICKOS_SERVICE_LIST=kickos_services_xmc4800relax (the
// xmc4800-relax enforcement default); the console comes up via this list (its first
// KOS_SVC_CONSOLE entry). EXACTLY ONE kickos_board_services links per image.
//
// This combined list lives with the SSC (SPI) driver (not the UART driver) because it
// now spans both; the CMake target links kickos_xmcuart AND kickos_xmcssc so both
// drivers + their class leaves come along.

#include <kickos/sys/service.h>

#include <kickos/driver/xmcuart.h>
#include <kickos/driver/xmcssc.h>

extern "C"
{
    // USIC0 CH0 @ 0x4003_0000, 0x200 B (RM Table 18-21). prio 12 matches the demo's
    // DRIVER_PRIO and must be >= every stdout client's priority (D9: no PI on the
    // console rendezvous).
    static struct kos_service_cfg const xmcuart_cfg = {
        /*name=*/"xmcuart", /*mmio_base=*/0x40030000u, /*mmio_window=*/0x200u,
        /*hz=*/0, /*addr=*/0, /*prio=*/12, /*kind=*/KOS_SVC_CONSOLE,
        /*cs_policy=*/KOS_SVC_CS_NONE, /*cs_index=*/0, /*rsv=*/{ 0, 0 }
    };

    // USIC0 CH1 @ 0x4003_0200, 0x200 B window (RM Table 18-21; the 0x200-aligned pow2
    // window that encodes on PMSA as one exact-cover descriptor). hz is informational
    // (the baud profile is fixed and PV-write-only; the driver reports its nominal
    // rate). CS_HW: the controller's own MSLS/SELO0 line, held across the transaction
    // by PCR.FEM=1. The call/reply path donates the caller's priority to the driver,
    // so its static prio is a floor, not the served priority.
    static struct kos_service_cfg const xmcssc_cfg = {
        /*name=*/"xmcssc", /*mmio_base=*/0x40030200u, /*mmio_window=*/0x200u,
        /*hz=*/1000000u, /*addr=*/0, /*prio=*/11, /*kind=*/KOS_SVC_SPI,
        /*cs_policy=*/KOS_SVC_CS_HW, /*cs_index=*/0, /*rsv=*/{ 0, 0 }
    };

    static struct kos_service_bringup const xmc4800relax_services[] = {
        { xmcuart_console_start, &xmcuart_cfg },
        { xmc_spi0_start, &xmcssc_cfg },
    };
    struct kos_service_list const kickos_board_services = { xmc4800relax_services, 2 };
}
