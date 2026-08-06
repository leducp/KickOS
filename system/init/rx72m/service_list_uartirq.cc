// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// rx72m IRQ-CONSOLE service-list provider: one entry, KOS_SVC_CONSOLE ->
// rxsci_console_start (SCI6 handover to the buffered IRQ-driven driver). The board
// default stays kickos_services_none; this list is selected ONLY by an explicit
// -DKICKOS_SERVICE_LIST=kickos_services_rx72m_uartirq, and no part of the driver has run
// on silicon (EXACTLY ONE kickos_board_services links per image).
//
// The bring-up needs AUTH_MEMORY + AUTH_CONSOLE + AUTH_IRQ, which root still holds
// because it runs from kickos_init_entry BEFORE kickos_default_init_run narrows root's
// authority. No app needs KOS_AUTH_IRQ.

#include <kickos/sys/service.h>
#include <kickos/chip_mmap.h>

#include <rxsci.h>

extern "C"
{
    // SCI6 @ 0x0008_A0C0, 16 B (UM sec.42): the SFR aperture is MPU-checked in user mode,
    // so the window is real enforcement here and 16 B is exactly one RX MPU page. prio 12
    // is the SERVICE thread and must be >= every stdout client (D9: no PI on the console
    // rendezvous); the driver spawns its IRQ thread and its RXI6 relay at prio + 1, so 12
    // must leave one priority above it free. hz is 0 because the driver does not
    // reprogram the baud divisor on a live channel: it inherits the kernel console's rate.
    static struct kos_service_cfg const rxsci_cfg = {
        /*name=*/"rxsci", /*mmio_base=*/kickos::rx::mmap::SCI6, /*mmio_window=*/16u,
        /*hz=*/0, /*addr=*/0, /*prio=*/12, /*kind=*/KOS_SVC_CONSOLE,
        /*rsv=*/{ 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const rx72m_uartirq_services[] = {
        { rxsci_console_start, &rxsci_cfg },
    };
    struct kos_service_list const kickos_board_services = { rx72m_uartirq_services, 1 };
}
