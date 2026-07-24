// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// frdmk64f service-list provider: the board's kickos_board_services, brought up in
// array order by the default init BEFORE the app's main. Two entries:
//   [0] KOS_SVC_CONSOLE -> k64uart_console_start (UART0 handover; console FIRST so
//       the app's stdout reaches the wire before anything else runs).
//   [1] KOS_SVC_SPI     -> k64dspi_spi_start     (DSPI0 bus service on an endpoint).
// Per-instance config travels as DATA (kos_service_cfg), never as literals in a
// driver TU. Selected by KICKOS_SERVICE_LIST=kickos_services_frdmk64f (the frdmk64f
// enforcement default); the board's console hook is kickos_console_none, so the
// console comes up via this list. EXACTLY ONE kickos_board_services links per image.
//
// This combined list lives with the DSPI driver (not the UART driver) because it now
// spans both; the CMake target links kickos_k64uart AND kickos_k64dspi so both
// drivers + their class leaves come along.

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
        /*cs_policy=*/KOS_SVC_CS_NONE, /*cs_index=*/0, /*rsv=*/{ 0, 0 }
    };

    // DSPI0 @ 0x4002_C000, 0x40 B window (RM ch.50; AIPS0 slot 44; the 32-aligned
    // pow2 window that encodes on SYSMPU/PMSA/PMP alike). hz 10 MHz = the LAN9252
    // boot rate; the client can re-tune via a CONFIG op. CS_GPIO on PTC4 (D9). The
    // call/reply path donates the caller's priority to the driver, so its static
    // prio is a floor, not the served priority.
    static struct kos_service_cfg const k64dspi_cfg = {
        /*name=*/"k64dspi", /*mmio_base=*/0x4002C000u, /*mmio_window=*/0x40u,
        /*hz=*/10000000u, /*addr=*/0, /*prio=*/11, /*kind=*/KOS_SVC_SPI,
        /*cs_policy=*/KOS_SVC_CS_GPIO, /*cs_index=*/4, /*rsv=*/{ 0, 0 }
    };

    static struct kos_service_bringup const frdmk64f_services[] = {
        { k64uart_console_start, &k64uart_cfg },
        { k64dspi_spi_start, &k64dspi_cfg },
    };
    struct kos_service_list const kickos_board_services = { frdmk64f_services, 2 };
}
