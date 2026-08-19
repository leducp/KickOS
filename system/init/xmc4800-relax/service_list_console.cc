// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// xmc4800-relax CONSOLE-ONLY service-list provider: one entry, KOS_SVC_CONSOLE ->
// xmcuart_console_start (USIC0 CH0 handover). This directory's service_list.cc also brings
// the USIC0 CH1 SSC bus up; the two are alternatives, never both linked (EXACTLY ONE
// kickos_board_services per image).
//
// An opt-in alternative, never the board default: boards/xmc4800-relax/Kconfig defaults
// KICKOS_SERVICE_LIST to the combined kickos_services_xmc4800relax at enforcement, so this
// one needs -DKICKOS_SERVICE_LIST=kickos_services_xmc4800relax_console. The SSC bring-up
// needs a privileged root, so an image for an unprivileged root must not link that driver
// at all rather than link it and not call it. The console bring-up is pure syscall and
// touches no register from the calling thread, so AUTH_MEMORY + AUTH_CONSOLE on an
// unprivileged root is enough.

#include <kickos/sys/service.h>
#include <kickos/chip_mmap.h>

#include <kickos/driver/xmcuart.h>

extern "C"
{
    // USIC0 CH0 @ 0x4003_0000, 0x200 B (RM Table 18-21): the 0x200-aligned pow2
    // window that encodes on PMSA as one exact-cover descriptor, leaving the sibling
    // channel U0C1 (base + 0x200) and the SCU / IOCR peripherals outside it. prio 12
    // must be >= every stdout client's priority (D9: no PI on the console rendezvous).
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

    static struct kos_service_bringup const xmc4800relax_console_services[] = {
        { xmcuart_console_start, &xmcuart_cfg },
    };
    struct kos_service_list const kickos_board_services = { xmc4800relax_console_services, 1 };
}
