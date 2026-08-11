# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_xmcuart: the XMC4800 userspace polled UART TX console driver (an
# unprivileged driver thread + a privileged one-shot handover bring-up; see
# <kickos/driver/xmcuart.h>). REGDIR carries regs/usic.h (the shared USIC
# offset+bit-field header) - definitions only, no code. The transmit-ready read is
# shared USIC register logic, so it links the kickos_class_xmc4800 leaf.
#
# TWO SOURCES. ../uart_usic.cc is the chip's ONE backend of the raw UART class
# <kickos/driver/uart.h> and owns every register; xmcuart.cc is the console consumer
# calling it. kickos_xmcuartirq compiles the SAME backend, so an image may link only one
# of the two drivers.
kickos_add_driver(xmcuart SOURCES xmcuart.cc ../uart_usic.cc
                  CLASS kickos_class_xmc4800 REGDIR arch/arm/chip/xmc4800)
