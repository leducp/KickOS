# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_k64uartirq: the K64F/UART0 buffered IRQ-driven userspace UART driver (an
# unprivileged IRQ thread owning the register window + an unprivileged service thread
# owning the endpoint, plus the privileged one-shot bring-up; see k64uartirq.h).
#
# TWO SOURCES. ../uart_k64.cc is the chip's ONE backend of the raw UART class
# <kickos/driver/uart.h> and owns every register; k64uartirq.cc is the service composed on
# it.
#
# REGDIR carries the chip's regs/uart.h bit map and irq.h vector numbers (definitions
# only, no code). CLASS carries the transmit-ready read, which is shared UART register
# logic (decision R-A), not a local copy.
#
# LEVEL: every source on this vector is a status flag that stays asserted until the driver
# clears it at the peripheral.
kickos_add_driver(k64uartirq SOURCES k64uartirq.cc ../uart_k64.cc
                  REGDIR arch/arm/chip/mk64f CLASS kickos_class_mk64f
                  BASE mmap::UART0_BASE WINDOWS regs LINES rxtx:level
                  BLOCK ${KICKOS_UART_BLOCK_SIZE} INIT block_init BARRIER 1 READY uart::KOS_UART_READY_OFFSET
                  POSTURE handover CONSOLE START k64uartirq_console_start
                  THREAD uartirq PRIORITY 1 ENTRY irq_entry ARG block WINDOW regs CAPS notify:wait rxtx:wait
                  THREAD service ENTRY uart::console_thread ARG block CAPS ep:wait notify:signal:doorbell)
