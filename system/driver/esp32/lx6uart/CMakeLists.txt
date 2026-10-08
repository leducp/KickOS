# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_lx6uart: the ESP32 (Xtensa LX6) UART0 buffered userspace UART driver - a
# privileged one-shot bring-up plus two unprivileged threads on the
# <kickos/sys/uart_service.h> substrate. REGDIR carries regs/uart.h + regs/system.h, register
# facts only, no code, and the chip file's generated irq.h and chip_mmap.h the rest. No class
# leaf: nothing here is shared with the kernel side.
#
# TWO SOURCES, and the split is the driver-model ruling: uart_lx6.cc is the LX6 backend of
# the raw UART class <kickos/driver/uart.h> and owns every register; lx6uart.cc is the
# service composed on it. An image gets exactly one definition of that class, so a second
# esp32 UART driver may not be linked beside this one.
#
# LEVEL: CPU interrupt 13 is level-triggered (TRM Table 8.3-2) and the UART latch stays set until
# the driver clears it.
kickos_add_driver(lx6uart SOURCES lx6uart.cc uart_lx6.cc ../../espuart/uart_esp.cc REGDIR arch/xtensa/chip/esp32
                  BASE mmap::UART0_BASE WINDOWS regs LINES uart0:level
                  BLOCK ${KICKOS_UART_BLOCK_SIZE} INIT block_init BARRIER 1 READY uart::KOS_UART_READY_OFFSET
                  POSTURE handover CONSOLE START lx6uart_console_start
                  THREAD lx6uartirq PRIORITY 1 ENTRY irq_entry ARG block WINDOW regs CAPS notify:wait uart0:wait
                  THREAD service ENTRY uart::console_thread ARG block CAPS ep:wait notify:signal:doorbell)
