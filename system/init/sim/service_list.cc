// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host-sim service-list provider: the sim console driver, kickos_simcon, owning the console.
// Selected with -DKICKOS_SERVICE_LIST=kickos_services_sim; the sim default stays
// kickos_services_none so the ordinary sim gate keeps testing the pre-publish route.

#include <kickos/sys/service.h>

extern "C"
{
    int simcon_console_start(struct kos_service_cfg const* cfg);

    // prio 12 matches the silicon console services: it must sit at or above every
    // stdout client's priority (there is no PI on the console rendezvous).
    static struct kos_service_cfg const simcon_cfg = {
        .name = "simcon",
        .mmio_base = 0,
        .mmio_window = 0,
        .hz = 0,
        .addr = 0,
        .prio = 12,
        .kind = KOS_SVC_CONSOLE,
        .rsv = { 0, 0, 0, 0 },
        .instance = nullptr
    };

    static struct kos_service_bringup const sim_services[] = {
        { simcon_console_start, &simcon_cfg },
    };
    struct kos_service_list const kickos_board_services = { sim_services, 1 };
}
