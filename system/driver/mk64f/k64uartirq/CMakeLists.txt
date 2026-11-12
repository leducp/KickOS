# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_k64uartirq: the K64F UART0 buffered console driver, an IRQ thread holding the register
# window and a service thread holding the endpoint. ../uart_k64.cc is the chip's backend of
# <kickos/driver/uart.h>, and k64uartirq.cc composes the service on it. REGDIR carries the
# chip's regs/uart.h and irq.h, CLASS the transmit-ready read.
#
# LEVEL: every source on both vectors is a status flag that stays asserted until the driver
# clears it at the peripheral. err carries S1.OR, which can stay set with RDRF clear and then
# raises nothing on rxtx (../uart_k64.cc).
kickos_add_driver(k64uartirq SOURCES k64uartirq.cc ../uart_k64.cc
                  REGDIR arch/arm/chip/mk64f CLASS kickos_class_mk64f
                  BASE mmap::UART0_BASE WINDOWS regs LINES rxtx:level err:level
                  BLOCK ${KICKOS_UART_BLOCK_SIZE} INIT block_init BARRIER 1 READY uart::KOS_UART_READY_OFFSET
                  POSTURE handover CONSOLE START k64uartirq_console_start
                  THREAD uartirq PRIORITY 1 ENTRY irq_entry ARG block WINDOW regs CAPS notify:wait rxtx:wait err:wait
                  THREAD service ENTRY uart::console_thread ARG block CAPS ep:wait notify:signal:doorbell)
