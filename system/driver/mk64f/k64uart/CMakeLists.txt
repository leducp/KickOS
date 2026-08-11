# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_k64uart: the K64F/UART0 userspace polled UART TX console driver (an
# unprivileged driver thread + a privileged one-shot handover bring-up; see
# <kickos/driver/k64uart.h>).
#
# TWO SOURCES. ../uart_k64.cc is the chip's ONE backend of the raw UART class
# <kickos/driver/uart.h> and owns every register; k64uart.cc is the console consumer
# calling it. kickos_k64uartirq compiles the SAME backend, so an image may link only one of
# the two drivers. REGDIR carries the chip's regs/uart.h bit map (definitions only, no
# code); CLASS carries the transmit-ready read, which is shared UART register logic
# (decision R-A), not a local copy.
kickos_add_driver(k64uart SOURCES k64uart.cc ../uart_k64.cc
                  REGDIR arch/arm/chip/mk64f CLASS kickos_class_mk64f)
