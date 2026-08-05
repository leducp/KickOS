// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// pizero2350 USB-CDC-CONSOLE service-list provider: one entry, KOS_SVC_CONSOLE ->
// rpusb_console_start (a CDC-ACM console over the RP2350 USB device controller). The
// board default stays kickos_services_none; this list is selected ONLY by an explicit
// -DKICKOS_SERVICE_LIST=kickos_services_pizero2350_usbcdc, and nothing about the USB
// driver has run on silicon (EXACTLY ONE kickos_board_services links per image).
//
// The kernel console on this board is UART1 on GP4/GP5, a DIFFERENT peripheral from the
// one the driver takes, so publishing blinds a working pin UART and the board is silent
// with no USB host attached. docs/design-m4.6.2-usb-cdc.md section 6.2 rules that a
// disjoint-device console must fall back to KERNEL_OWNED on driver death instead of
// RECLAIMED; that kernel delta is NOT implemented, so expect the FTDI on GP4/GP5 to go
// quiet at the handover.
//
// The bring-up needs AUTH_MEMORY + AUTH_CONSOLE + AUTH_IRQ, which root still holds
// because it runs from kickos_init_entry BEFORE kickos_default_init_run narrows root's
// authority.

#include <kickos/sys/service.h>

extern "C"
{
    // Declared here: kickos_rpusb exports no header.
    int rpusb_console_start(struct kos_service_cfg const* cfg);

    // USB DPRAM at 0x5010_0000 with the register block 0x10000 above it. The window must
    // be a POWER OF TWO, naturally aligned: arch_mpu_region_encodable gates every MMIO
    // spawn and is NOT compiled out at KICKOS_HAVE_MPU=0, where this board links the
    // v7-M encoder (arch_arm_pmsav8.cc is inside the enforcement guard). A byte-granular
    // 0x11000 is admissible only in the PMSAv8 posture and refuses the spawn -KOS_EINVAL
    // in the default one. hz is unused: a CDC line rate is set by the host over the
    // control endpoint and there is no baud generator behind it. prio 12 is the SERVICE
    // thread and must be >= every stdout client; the driver spawns its IRQ thread at
    // prio + 1, so 12 must leave one priority above it free.
    static struct kos_service_cfg const rpusb_cfg = {
        /*name=*/"rpusb", /*mmio_base=*/0x50100000u, /*mmio_window=*/0x20000u,
        /*hz=*/0u, /*addr=*/0, /*prio=*/12, /*kind=*/KOS_SVC_CONSOLE,
        /*rsv=*/{ 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const pizero2350_usbcdc_services[] = {
        { rpusb_console_start, &rpusb_cfg },
    };
    struct kos_service_list const kickos_board_services = { pizero2350_usbcdc_services, 1 };
}
