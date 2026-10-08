# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_rxsci: the RX72M SCI6 buffered IRQ-driven userspace UART/console driver (an
# unprivileged two-thread driver plus a privileged one-shot handover bring-up; see
# rxsci.h). REGDIR carries the chip's clean-room mmap.h + regs/sci.h, register facts
# only, so they are not duplicated per driver.
#
# TWO SOURCES, and the split is the driver-model ruling: ../uart_sci.cc is the RX72M
# backend of the raw UART class <kickos/driver/uart.h> and owns every register;
# rxsci.cc is the service composed on it. It sits at chip level so a second RX72M SCI
# driver would compile the SAME backend, and an image gets exactly one definition of
# that class.
#
# Both lines EDGE: a raise taken while a line is masked latches and redelivers on the rearm.
# TEI6 and ERI6 are LEVEL and are not claimed (see rxsci.h). One wait covers both lines and the
# doorbell, so the window holder services RXI itself.
kickos_add_driver(rxsci SOURCES rxsci.cc ../uart_sci.cc REGDIR arch/rx/chip/rx72m
                  BASE mmap::SCI6 WINDOWS regs LINES txi:edge rxi:edge
                  BLOCK ${KICKOS_UART_BLOCK_SIZE} INIT block_init BARRIER 1 READY uart::KOS_UART_READY_OFFSET
                  POSTURE handover CONSOLE START rxsci_console_start
                  THREAD rxsciirq PRIORITY 1 ENTRY irq_entry ARG block WINDOW regs CAPS notify:wait txi:wait rxi:wait
                  THREAD service ENTRY uart::console_thread ARG block CAPS ep:wait notify:signal:doorbell)
