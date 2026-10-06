// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host-sim service list carrying the loopback UART, kickos_simuart. Selected with
// -DKICKOS_SERVICE_LIST=kickos_services_simuart.
//
// This list deliberately publishes NO console. It is a KOS_SVC_UART port, so the kernel
// keeps its own console and both paths stay readable.

#include <kickos/sys/service.h>

extern "C"
{
    int simuart_start(struct kos_service_cfg const* cfg);

    static struct kos_service_cfg const simuart_cfg = {
        .name = "simuart",
        .mmio_base = 0,
        .mmio_window = 0,
        .hz = 0,
        .addr = 0,
        .prio = 12,
        .kind = KOS_SVC_UART,
        .rsv = { 0, 0, 0, 0 },
        .instance = nullptr
    };

    static struct kos_service_bringup const simuart_services[] = {
        {simuart_start, &simuart_cfg},
    };
    struct kos_service_list const kickos_board_services = {simuart_services, 1};
}
