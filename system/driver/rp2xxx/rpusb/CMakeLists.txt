# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_rpusb: the RP2040/RP2350 USB CDC-ACM console driver (a privileged one-shot
# bring-up plus two unprivileged threads on the <kickos/sys/usb_cdc_service.h>
# substrate). No class leaf: the USB register map is the driver's alone; the kernel side
# owns only RESETS, CLOCKS and PLL_USB.
kickos_add_driver(rpusb
                  THREADS rpusbirq:1:default:2:0 service:0:default:2:1 RECEIVER service
                  WINDOWS regs LINES irq NOTIFY BLOCK ${KICKOS_USB_BLOCK_SIZE}
                  POSTURE handover BARRIER 1 START rpusb_console_start CONSOLE USB_DEVICE)
