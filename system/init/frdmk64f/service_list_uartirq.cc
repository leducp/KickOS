// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// frdmk64f IRQ-CONSOLE service-list provider: one entry, KOS_SVC_CONSOLE ->
// k64uartirq_console_start (UART0 handover to the two-thread buffered driver). The
// shipping list is service_list_frdmk64f.cc (polled UART0 console + DSPI0 bus); EXACTLY
// ONE kickos_board_services links per image.
//
// Selected ONLY by an explicit -DKICKOS_SERVICE_LIST=kickos_services_frdmk64f_uartirq;
// the board default stays the polled list, which has run on silicon where this driver
// has not.
//
// The bring-up needs AUTH_MEMORY + AUTH_CONSOLE + AUTH_IRQ, which root still holds
// because it runs from kickos_init_entry BEFORE kickos_default_init_run narrows root's
// authority. No app needs KOS_AUTH_IRQ.

#include <kickos/sys/service.h>
#include <kickos/chip_mmap.h>

#include <k64uartirq.h>

extern "C"
{
    // UART0 @ 0x4006_A000, 0x20 B (RM ch.52; AIPS0 slot 106). prio 12 is the SERVICE
    // thread and must be >= every stdout client (D9: no PI on the console rendezvous);
    // the driver spawns its IRQ thread at prio + 1, so 12 must leave one priority above
    // it free.
    static struct kos_service_cfg const k64uartirq_cfg = {
        /*name=*/"k64uartirq", /*mmio_base=*/kickos::mk64f::mmap::UART0_BASE,
        /*mmio_window=*/0x20u, /*hz=*/115200u, /*addr=*/0, /*prio=*/12, /*kind=*/KOS_SVC_CONSOLE,
        /*rsv=*/{ 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const frdmk64f_uartirq_services[] = {
        { k64uartirq_console_start, &k64uartirq_cfg },
    };
    struct kos_service_list const kickos_board_services = { frdmk64f_uartirq_services, 1 };
}
