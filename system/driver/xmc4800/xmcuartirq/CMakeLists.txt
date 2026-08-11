# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_xmcuartirq: the XMC4800 IRQ-driven buffered UART console driver on USIC0 CH0
# (two unprivileged threads over the shared <kickos/sys/uart_service.h> substrate, plus a
# one-shot handover bring-up; see <kickos/driver/xmcuartirq.h>). REGDIR carries regs/usic.h
# and irq.h (the shared USIC offset+bit-field header and the NVIC node numbers):
# definitions only, no code. The transmit-ready read is shared USIC register logic, so it
# links the kickos_class_xmc4800 leaf.
#
# TWO SOURCES, and the split is the driver-model ruling: ../uart_usic.cc is the USIC backend
# of the raw UART class <kickos/driver/uart.h> and owns every register; xmcuartirq.cc is the
# service composed on it. It sits at chip level because kickos_xmcuart compiles the SAME
# backend. An image gets exactly one definition of that class, so a second xmc4800 UART
# driver may not be linked beside this one.
kickos_add_driver(xmcuartirq SOURCES xmcuartirq.cc ../uart_usic.cc
                  CLASS kickos_class_xmc4800 REGDIR arch/arm/chip/xmc4800)

# The driver header ships beside the driver rather than in user/include, so a service-list
# provider that links this target reaches it as <kickos/driver/xmcuartirq.h>.
target_include_directories(kickos_xmcuartirq PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
