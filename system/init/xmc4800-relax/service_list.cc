// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// xmc4800-relax service-list provider, brought up in array order by the default init
// BEFORE the app's main. The console entry MUST stay first, so the app's stdout reaches
// the wire before anything else runs. Selected by
// KICKOS_SERVICE_LIST=kickos_services_xmc4800relax (the xmc4800-relax enforcement
// default); its CMake target links kickos_xmcuart AND kickos_xmcssc. EXACTLY ONE
// kickos_board_services links per image.

#include <kickos/sys/service.h>
#include <kickos/chip_mmap.h>

#include <kickos/driver/xmcuart.h>
#include <kickos/driver/xmcssc.h>

extern "C"
{
    // USIC0 CH0 @ 0x4003_0000, 0x200 B (RM Table 18-21). prio 12 must be >= every stdout
    // client's priority (D9: no PI on the console rendezvous).
    static struct kos_service_cfg const xmcuart_cfg = {
        .name = "xmcuart",
        .mmio_base = kickos::xmc::mmap::USIC0_CH0_BASE,
        .mmio_window = 0x200u,
        .hz = 0,
        .addr = 0,
        .prio = 12,
        .kind = KOS_SVC_CONSOLE,
        .rsv = { 0, 0, 0, 0 }
    };

    // USIC0 CH1 @ 0x4003_0200, 0x200 B window (RM Table 18-21; the 0x200-aligned pow2
    // window that encodes on PMSA as one exact-cover descriptor). hz is informational:
    // the baud profile is fixed at bring-up. CS_HW is the controller's own MSLS/SELO0
    // line, held across the transaction by PCR.FEM=1. The call/reply path donates the
    // caller's priority to the driver, so its static prio is a floor, not the served one.
    static struct kos_service_cfg const xmcssc_cfg = {
        .name = "xmcssc",
        .mmio_base = kickos::xmc::mmap::USIC0_CH1_BASE,
        .mmio_window = 0x200u,
        .hz = 1000000u,
        .addr = 0,
        .prio = 11,
        .kind = KOS_SVC_SPI,
        .rsv = { 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const xmc4800relax_services[] = {
        { xmcuart_console_start, &xmcuart_cfg },
        { xmc_spi0_start, &xmcssc_cfg },
    };
    struct kos_service_list const kickos_board_services = { xmc4800relax_services, 2 };
}
