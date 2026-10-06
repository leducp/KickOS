# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The i.MX RT1062 USB CDC-ACM console driver.
kickos_add_driver(rt1062usb SOURCES rt1062usb.cc
                  THREADS rtusbirq:1:default:2:0 service:0:default:2:1 RECEIVER service
                  WINDOWS regs LINES irq NOTIFY BLOCK ${KICKOS_RT1062USB_BLOCK_SIZE} BLOCK_CACHE uncached
                  POSTURE handover BARRIER 1 START rtusb_console_start CONSOLE USB_DEVICE)

# Off unless a bench run asks for it: makes the IRQ thread read a kernel-only address, so a
# capture says whether an enforcing MPU refuses this thread at all.
if(KICKOS_RTUSB_UNGRANTED_PROBE)
    target_compile_definitions(kickos_rt1062usb PRIVATE KICKOS_RTUSB_UNGRANTED_PROBE=1)
endif()
