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
# service composed on it. It sits at chip level beside the other drivers over that USIC
# backend. An image gets exactly one definition of that class, so a second xmc4800 UART
# driver may not be linked beside this one.
#
# EDGE, with no peripheral-side clear to pair with it: PSR.TBIF has no influence on interrupt
# generation and does not need clearing (RM 18.2.2.3 p.18-17).
kickos_add_driver(xmcuartirq SOURCES xmcuartirq.cc ../uart_usic.cc
                  CLASS kickos_class_xmc4800 REGDIR arch/arm/chip/xmc4800
                  BASE mmap::USIC0_CH0_BASE WINDOWS regs LINES irq:edge
                  BLOCK ${KICKOS_UART_BLOCK_SIZE} INIT block_init BARRIER 1 READY uart::KOS_UART_READY_OFFSET
                  POSTURE handover CONSOLE START xmcuartirq_console_start
                  THREAD uartirq PRIORITY 1 ENTRY irq_entry ARG block WINDOW regs CAPS notify:wait irq:wait
                  THREAD service ENTRY uart::console_thread ARG block CAPS ep:wait notify:signal:doorbell)

target_include_directories(kickos_xmcuartirq PRIVATE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
