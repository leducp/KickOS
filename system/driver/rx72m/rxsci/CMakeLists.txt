# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_rxsci: the RX72M SCI6 buffered IRQ-driven userspace UART/console driver (an
# unprivileged three-thread driver plus a privileged one-shot handover bring-up; see
# rxsci.h). REGDIR carries the chip's clean-room mmap.h + regs/sci.h, register facts
# only, so they are not duplicated per driver.
#
# TWO SOURCES, and the split is the driver-model ruling: ../uart_sci.cc is the RX72M
# backend of the raw UART class <kickos/driver/uart.h> and owns every register;
# rxsci.cc is the service composed on it. It sits at chip level so a second RX72M SCI
# driver would compile the SAME backend, and an image gets exactly one definition of
# that class.
kickos_add_driver(rxsci SOURCES rxsci.cc ../uart_sci.cc REGDIR arch/rx/chip/rx72m)

# A service-list provider needs the bring-up declaration, and it lives beside the
# driver rather than in user/include: nothing outside a provider calls it.
target_include_directories(kickos_rxsci PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
