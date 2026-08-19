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
kickos_add_driver(f4uartirq SOURCES f4uartirq.cc ../uart_stm32f4.cc
                  REGDIR arch/arm/chip/stm32f411 CLASS kickos_class_stm32f411)

# The bring-up entry is declared beside the driver rather than in user/include, so a
# service-list provider that links this target also sees the header.
target_include_directories(kickos_f4uartirq INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
