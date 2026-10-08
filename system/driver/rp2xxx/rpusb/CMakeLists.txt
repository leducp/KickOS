# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_rpusb: the RP2040/RP2350 USB CDC-ACM console driver (a privileged one-shot
# bring-up plus two unprivileged threads on the <kickos/sys/usb_cdc_service.h>
# substrate). No class leaf: the USB register map is the driver's alone; the kernel side
# owns only RESETS, CLOCKS and PLL_USB.
#
# BASE: the register map is hard-wired to the one USB block, so an instance naming another window
# would grant one region and poke another. LEVEL: INTS is a pure OR of sources cleared at the
# peripheral, and its BUFF_STATUS bit stays asserted until every BUFF_STATUS bit is clear. The
# poll does not wait for enumeration, or boot would depend on a cable being plugged in.
#
# HANDOVER is the WRONG posture on both boards: the kernel console is a pin UART, a different
# peripheral, so the publish blinds a working UART. The ruled fallback to KERNEL_OWNED is a kernel
# delta, NOT implemented, and lands as a third posture.
kickos_add_driver(rpusb
                  BASE reg::DPRAM_BASE WINDOWS regs LINES irq:level
                  BLOCK ${KICKOS_USB_BLOCK_SIZE} INIT block_init BARRIER 1 READY usb::KOS_USB_READY_OFFSET
                  POSTURE handover CONSOLE USB_DEVICE START rpusb_console_start
                  THREAD rpusbirq PRIORITY 1 ENTRY rpusb_irq_thread ARG block WINDOW regs CAPS notify:wait irq:wait
                  THREAD service ENTRY rpusb_service_thread ARG block CAPS ep:wait notify:signal:doorbell)
