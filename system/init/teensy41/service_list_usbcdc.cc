// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// teensy41 USB-CDC-CONSOLE service-list provider: one entry, KOS_SVC_CONSOLE ->
// rtusb_console_start. Selected ONLY by an explicit
// -DKICKOS_SERVICE_LIST=kickos_services_teensy41_usbcdc, and exactly one
// kickos_board_services links per image.
//
// The kernel console is LPUART6 on pins 0/1, a different peripheral, so publishing blinds it.
//
// Selecting this list turns the M7 L1 D-cache off (derived in arch/CMakeLists.txt, which
// warns at configure): the controller is a bus master with no cache maintenance.

#include <kickos/sys/service.h>
#include <kickos/chip_mmap.h>

extern "C"
{
    int rtusb_console_start(struct kos_service_cfg const* cfg);

    // 512 B, not the 16 KiB Table 3-1 gives the whole peripheral: RM 42.7 addresses the
    // core as base + (512 * i). Do not widen it. arch_periph_enable opens the bus gate for
    // the WHOLE AIPS slot, so this window and the reserved remainder are the only things
    // keeping OTG2 at +0x200 and USBNC at +0x800 out of reach.
    static struct kos_service_cfg const rtusb_cfg = {
        .name = "rtusb",
        .mmio_base = kickos::imxrt1062::mmap::USB1_BASE,
        .mmio_window = 0x200u,
        .hz = 0u,
        .addr = 0,
        .prio = 12,
        .kind = KOS_SVC_CONSOLE,
        .rsv = { 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const teensy41_usbcdc_services[] = {
        { rtusb_console_start, &rtusb_cfg },
    };
    struct kos_service_list const kickos_board_services = { teensy41_usbcdc_services, 1 };
}
