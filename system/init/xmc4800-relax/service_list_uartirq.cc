// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// xmc4800-relax IRQ-CONSOLE service-list provider: USIC0 CH0 handover to the two-thread
// buffered driver. The polled alternatives are service_list_xmc4800relax_console.cc and
// service_list_xmc4800relax.cc; EXACTLY ONE kickos_board_services links per image, and
// this one is selected only by
// -DKICKOS_SERVICE_LIST=kickos_services_xmc4800relax_uartirq.
//
// The bring-up needs AUTH_MEMORY + AUTH_CONSOLE + AUTH_IRQ, which root still holds only
// because it runs from kickos_init_entry BEFORE kickos_default_init_run narrows root's
// authority.

#include <kickos/sys/service.h>
#include <kickos/chip_mmap.h>

#include <kickos/driver/xmcuartirq.h>

extern "C"
{
    // USIC0 CH0 @ 0x4003_0000, 0x200 B (RM Table 18-21): a 0x200-aligned pow2 window
    // encodes on PMSA as one exact-cover descriptor, leaving the sibling channel U0C1
    // (base + 0x200) and the SCU / IOCR peripherals outside it. prio 12 is the SERVICE
    // thread and must be >= every stdout client (D9: no PI on the console rendezvous); the
    // IRQ thread runs at prio + 1, so 12 must leave one priority above it free. hz must be
    // 0: FDR and BRG carry an allowlist entry on U0C1 only, so any nonzero hz fails the
    // bring-up.
    static struct kos_service_cfg const xmcuartirq_cfg = {
        .name = "xmcuartirq",
        .mmio_base = kickos::xmc::mmap::USIC0_CH0_BASE,
        .mmio_window = 0x200u,
        .hz = 0,
        .addr = 0,
        .prio = 12,
        .kind = KOS_SVC_CONSOLE,
        .rsv = { 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const xmc4800relax_uartirq_services[] = {
        { xmcuartirq_console_start, &xmcuartirq_cfg },
    };
    struct kos_service_list const kickos_board_services = { xmc4800relax_uartirq_services, 1 };
}
