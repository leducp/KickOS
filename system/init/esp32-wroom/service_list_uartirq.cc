// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// esp32-wroom (LX6) alternative service list: the IRQ-driven buffered console on UART0
// instead of the kernel's polled ring. NOT a board default; selected only with an
// explicit -DKICKOS_SERVICE_LIST=kickos_services_esp32_uartirq, and this driver has never
// run on silicon.

#include <kickos/sys/service.h>
#include <kickos/chip_mmap.h>

extern "C"
{

// Declared here: kickos_lx6uart exports no header.
int lx6uart_console_start(struct kos_service_cfg const* cfg);

// UART0 at 0x3FF4_0000, the block the ROM already brought up as the console. The window
// is the whole 4 KiB peripheral page: start() refuses any other base, because the class
// applies its offsets to whatever it is granted.
static struct kos_service_cfg const lx6uart_cfg = {
    .name = "lx6uart",
    .mmio_base = kickos::esp32::mmap::UART0_BASE,
    .mmio_window = 0x1000u,
    .hz = 0,
    .addr = 0,
    .prio = 12,
    .kind = KOS_SVC_CONSOLE,
    .rsv = { 0, 0, 0, 0 }
};

static struct kos_service_bringup const lx6uart_services[] = {
    {lx6uart_console_start, &lx6uart_cfg},
};
struct kos_service_list const kickos_board_services = {lx6uart_services, 1};
}
