// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// frdmk64f service-list provider, brought up in array order by the default init BEFORE
// the app's main. The console entry MUST stay first, so the app's stdout reaches the wire
// before anything else runs. Selected by KICKOS_SERVICE_LIST=kickos_services_frdmk64f
// (the frdmk64f enforcement default); its CMake target links kickos_k64uart AND
// kickos_k64dspi. EXACTLY ONE kickos_board_services links per image.

#include <kickos/sys/service.h>

#include <kickos/driver/k64uart.h>
#include <kickos/driver/k64dspi.h>

extern "C"
{
    // UART0 @ 0x4006_A000, 0x20 B (RM ch.52; AIPS0 slot 106). prio 12 >= every
    // stdout client (D9: no PI on the console rendezvous).
    static struct kos_service_cfg const k64uart_cfg = {
        /*name=*/"k64uart", /*mmio_base=*/0x4006A000u, /*mmio_window=*/0x20u,
        /*hz=*/0, /*addr=*/0, /*prio=*/12, /*kind=*/KOS_SVC_CONSOLE,
        /*rsv=*/{ 0, 0, 0, 0 }
    };

    // DSPI0 @ 0x4002_C000, 0x40 B window (RM ch.50; AIPS0 slot 44; the 32-aligned pow2
    // window that encodes on SYSMPU/PMSA/PMP alike). hz is informational and nothing reads
    // it: no boot rate is in effect, because spi_service.h refuses an XFER until a client
    // CONFIG op folds a profile, which is what programs CTAR0. The CS pin is hardcoded to
    // PTC4 in the driver. The call/reply path donates the caller's priority to the driver,
    // so its static prio is a floor, not the served priority.
    static struct kos_service_cfg const k64dspi_cfg = {
        /*name=*/"k64dspi", /*mmio_base=*/0x4002C000u, /*mmio_window=*/0x40u,
        /*hz=*/10000000u, /*addr=*/0, /*prio=*/11, /*kind=*/KOS_SVC_SPI,
        /*rsv=*/{ 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const frdmk64f_services[] = {
        { k64uart_console_start, &k64uart_cfg },
        { k64dspi_spi_start, &k64dspi_cfg },
    };
    struct kos_service_list const kickos_board_services = { frdmk64f_services, 2 };
}
