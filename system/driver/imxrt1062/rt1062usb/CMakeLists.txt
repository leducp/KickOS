# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The i.MX RT1062 USB CDC-ACM console driver.
#
# BASE: the register map is hard-wired to USB1, so an instance naming another window would grant
# one block and poke another. LEVEL: USBSTS is an OR of sources cleared at the peripheral, and
# the NVIC line follows it. Uncached: the controller reads its dQH/dTD lists and writes transfer
# results out of the block as a bus master, and this tree has no cache-maintenance primitive. The
# handover blinds the kernel console, LPUART6 on pins 0/1, a different peripheral from this one.
# irq_loop latches ready before enumeration, so the poll does not wait for a cable.
kickos_add_driver(rt1062usb SOURCES rt1062usb.cc
                  TAG rtusb BASE reg::USB1_BASE WINDOWS regs LINES irq:level
                  BLOCK ${KICKOS_RT1062USB_BLOCK_SIZE} BLOCK_CACHE uncached INIT block_init
                  BARRIER 1 READY READY_OFFSET
                  POSTURE handover CONSOLE USB_DEVICE START rtusb_console_start
                  THREAD rtusbirq PRIORITY 1 ENTRY rtusb_irq_thread ARG block WINDOW regs CAPS notify:wait irq:wait
                  THREAD service ENTRY rtusb_service_thread ARG block CAPS ep:wait notify:signal:doorbell)

# Off unless a bench run asks for it: makes the IRQ thread read a kernel-only address, so a
# capture says whether an enforcing MPU refuses this thread at all.
if(KICKOS_RTUSB_UNGRANTED_PROBE)
    target_compile_definitions(kickos_rt1062usb PRIVATE KICKOS_RTUSB_UNGRANTED_PROBE=1)
endif()
