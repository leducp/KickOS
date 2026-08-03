# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_k64uartirq: the K64F/UART0 buffered IRQ-driven userspace UART driver (an
# unprivileged IRQ thread owning the register window + an unprivileged service thread
# owning the endpoint, plus the privileged one-shot bring-up; see k64uartirq.h). The
# IRQ-driven counterpart of kickos_k64uart, which is polled and TX-only.
#
# REGDIR carries the chip's regs/uart.h bit map and irq.h vector numbers (definitions
# only, no code). CLASS carries the transmit-ready read, which is shared UART register
# logic (decision R-A), not a local copy.
kickos_add_driver(k64uartirq REGDIR arch/arm/chip/mk64f CLASS kickos_class_mk64f)

# The bring-up entry is declared beside the driver rather than in user/include, so a
# service-list provider that links this target also sees the header.
target_include_directories(kickos_k64uartirq INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
