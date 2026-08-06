// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// picopi USB-CDC-CONSOLE service-list provider: one entry, KOS_SVC_CONSOLE ->
// rpusb_console_start, the SAME driver the pizero2350 list uses. The board default stays
// kickos_services_none; this list is selected ONLY by an explicit
// -DKICKOS_SERVICE_LIST=kickos_services_picopi_usbcdc, and nothing about the USB driver
// has run on silicon (EXACTLY ONE kickos_board_services links per image).
//
// The kernel console here is UART0 on GP0/GP1, a different peripheral from the one the
// driver takes, so publishing blinds it exactly as on the pizero2350.
//
// RP2040-E5: the controller needs 800 us of idle J-state after a bus reset, and is
// hardware-fixed only in stepping B2. An earlier stepping behind a hub transaction
// translator never enumerates. No workaround is carried here; the bench Pico's stepping
// is open question 3 of docs/design-m4.6.2-usb-cdc.md.

#include <kickos/sys/service.h>
#include <kickos/chip_mmap.h>

extern "C"
{
    int rpusb_console_start(struct kos_service_cfg const* cfg);

    // USB DPRAM at 0x5010_0000 with the register block 0x10000 above it. The window is
    // 128 KiB because PMSAv6 needs a power-of-two naturally aligned region; 0x50100000 is
    // 128 KiB-aligned and the whole run stays inside the USB block's own AHB slot.
    static struct kos_service_cfg const rpusb_cfg = {
        .name = "rpusb",
        .mmio_base = kickos::rp2040::mmap::USBCTRL_DPRAM_BASE,
        .mmio_window = kickos::rp2040::mmap::USBCTRL_WINDOW,
        .hz = 0u,
        .addr = 0,
        .prio = 12,
        .kind = KOS_SVC_CONSOLE,
        .rsv = { 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const picopi_usbcdc_services[] = {
        { rpusb_console_start, &rpusb_cfg },
    };
    struct kos_service_list const kickos_board_services = { picopi_usbcdc_services, 1 };
}
