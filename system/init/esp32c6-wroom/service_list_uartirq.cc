// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// esp32c6-wroom IRQ-CONSOLE service-list provider: one entry, KOS_SVC_CONSOLE ->
// c6uart_console_start (UART0 handover to the two-thread buffered driver). The board
// default stays kickos_services_none; this list is selected ONLY by an explicit
// -DKICKOS_SERVICE_LIST=kickos_services_esp32c6_uartirq, and no part of the driver has
// run on silicon (EXACTLY ONE kickos_board_services links per image).
//
// The bring-up needs AUTH_MEMORY + AUTH_CONSOLE + AUTH_IRQ, which root still holds
// because it runs from kickos_init_entry BEFORE kickos_default_init_run narrows root's
// authority. No app needs KOS_AUTH_IRQ.

#include <kickos/sys/service.h>

extern "C"
{
    // Declared here: kickos_c6uart exports no header.
    int c6uart_console_start(struct kos_service_cfg const* cfg);

    // UART0 @ 0x6000_0000, 0x1000 B (TRM v1.2 ch.27): the 0x1000-aligned pow2 window that
    // encodes on PMP as one exact-cover entry, leaving UART1 (base + 0x1000) outside it.
    // The driver refuses any base but UART0: its register class is hard-wired to the
    // console channel. prio 12 is the SERVICE thread and must be >= every stdout client
    // (D9: no PI on the console rendezvous); the driver spawns its IRQ thread at prio + 1,
    // so 12 must leave one priority above it free.
    static struct kos_service_cfg const c6uart_cfg = {
        /*name=*/"c6uart", /*mmio_base=*/0x60000000u, /*mmio_window=*/0x1000u,
        /*hz=*/115200u, /*addr=*/0, /*prio=*/12, /*kind=*/KOS_SVC_CONSOLE,
        /*cs_policy=*/KOS_SVC_CS_NONE, /*cs_index=*/0, /*rsv=*/{ 0, 0 }
    };

    static struct kos_service_bringup const esp32c6_uartirq_services[] = {
        { c6uart_console_start, &c6uart_cfg },
    };
    struct kos_service_list const kickos_board_services = { esp32c6_uartirq_services, 1 };
}
