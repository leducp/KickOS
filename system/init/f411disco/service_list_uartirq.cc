// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// f411disco IRQ-CONSOLE service-list provider: one entry, KOS_SVC_CONSOLE ->
// f4uartirq_console_start (USART2 handover to the two-thread buffered driver). It is the
// board's only service list, so the board default stays kickos_services_none and this one is
// selected ONLY by -DKICKOS_SERVICE_LIST=kickos_services_f411disco_uartirq.
//
// The DRIVER is per-chip and blackpill shares it; this file is the per-BOARD half, and the
// only board fact in it is the choice of USART2 as the console channel, which both boards
// happen to make.
//
// The bring-up needs AUTH_MEMORY + AUTH_CONSOLE + AUTH_IRQ, which root still holds because
// it runs from kickos_init_entry BEFORE kickos_default_init_run narrows root's authority.
// No app needs KOS_AUTH_IRQ.

#include <kickos/sys/service.h>
#include <kickos/chip_mmap.h>

#include <f4uartirq.h>

extern "C"
{
    // USART2 @ 0x4000_4400, 0x20 B. The register file ends at GTPR, so it spans 0x00 to
    // 0x1B (RM0383 sec.19.6.8 Table 88) and 0x20 covers all of it with 4 bytes of slack.
    // It deliberately UNDER-covers the 1 KB APB1 slot the memory map gives USART2
    // (0x4000_4400 to 0x4000_47FF, RM0383 sec.2.3): nothing else lives in that slot, so the
    // narrower window aliases no neighbour, and 32 bytes is the PMSAv7 minimum region so it
    // cannot be tightened further. Naturally aligned at that base, so it encodes as one
    // descriptor.
    //
    // prio 12 is the SERVICE thread and must be >= every stdout client (D9: no PI on the
    // console rendezvous); the driver spawns its IRQ thread at prio + 1, so 12 must leave one
    // priority above it free.
    static struct kos_service_cfg const f4uartirq_cfg = {
        .name = "f4uartirq",
        .mmio_base = kickos::stm32f411::mmap::USART2_BASE,
        .mmio_window = 0x20u,
        .hz = 115200u,
        .addr = 0,
        .prio = 12,
        .kind = KOS_SVC_CONSOLE,
        .rsv = { 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const f411disco_uartirq_services[] = {
        { f4uartirq_console_start, &f4uartirq_cfg },
    };
    struct kos_service_list const kickos_board_services = { f411disco_uartirq_services, 1 };
}
