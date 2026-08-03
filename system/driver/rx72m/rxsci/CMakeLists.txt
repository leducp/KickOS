# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_rxsci: the RX72M SCI6 buffered IRQ-driven userspace UART/console driver (an
# unprivileged three-thread driver plus a privileged one-shot handover bring-up; see
# rxsci.h). REGDIR carries the chip's clean-room mmap.h + regs/sci.h -- definitions
# only, no code -- so the register facts are not duplicated per driver.
kickos_add_driver(rxsci REGDIR arch/rx/chip/rx72m)

# A service-list provider needs the bring-up declaration, and it lives beside the
# driver rather than in user/include: nothing outside a provider calls it.
target_include_directories(kickos_rxsci PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

# Bench localizer, -DRXSCI_TRACE=1. Target-scoped and safe to scope: the macro is read
# only inside rxsci.cc, never in a header, so no other TU can disagree about a type or
# a signature. See the legend at the top of rxsci.cc.
foreach(_k RXSCI_TRACE RXSCI_NO_RX RXSCI_LED_TRACE)
  if(${_k})
    target_compile_definitions(kickos_rxsci PRIVATE ${_k}=${${_k}})
  endif()
endforeach()
