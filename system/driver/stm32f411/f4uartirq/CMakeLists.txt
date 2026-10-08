# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_f4uartirq: the STM32F411/USART2 buffered IRQ-driven userspace UART driver (an
# unprivileged IRQ thread owning the register window + an unprivileged service thread owning
# the endpoint, plus the privileged one-shot bring-up; see f4uartirq.h).
#
# Two sources: ../uart_stm32f4.cc is the chip backend of <kickos/driver/uart.h> and owns every
# register, f4uartirq.cc is the service composed on it. The backend sits one level up because
# every stm32f411 UART driver compiles that same file.
#
# The class symbols are strong C definitions, so an image may link only ONE driver defining them.
#
# REGDIR carries the chip's regs/usart.h bit map and irq.h vector numbers, definitions only.
# CLASS carries the transmit-ready read, which is shared USART register logic.
#
# LEVEL: every USART event is ORed into one request line (RM0383 sec.19.4 Figure 191), so a
# status flag stays asserted until the driver clears it at the peripheral. ORE is the one that
# matters: RXNEIE arms it, and a bare DR read does not clear it.
kickos_add_driver(f4uartirq SOURCES f4uartirq.cc ../uart_stm32f4.cc
                  REGDIR arch/arm/chip/stm32f411 CLASS kickos_class_stm32f411
                  BASE mmap::USART2_BASE WINDOWS regs LINES global:level
                  BLOCK ${KICKOS_UART_BLOCK_SIZE} INIT block_init BARRIER 1 READY uart::KOS_UART_READY_OFFSET
                  POSTURE handover CONSOLE START f4uartirq_console_start
                  THREAD uartirq PRIORITY 1 ENTRY irq_entry ARG block WINDOW regs CAPS notify:wait global:wait
                  THREAD service ENTRY uart::console_thread ARG block CAPS ep:wait notify:signal:doorbell)
