# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# STM32F411 SPI1 loopback driver over the MMIO-grant seam (task #9 Stage 5): the
# CANONICAL per-thread peripheral-MMIO isolation reference on ARMv7-M PMSA -- a real
# SPI1 window granted to an unprivileged thread, proven both ways (granted works,
# ungranted faults MemManage). Diagnostic app: built only under KICKOS_ENABLE_SELFTEST
# (never a production image). Registered only for the f411disco board (chip-specific
# SPI1/GPIOA/RCC bring-up).
kickos_add_diagnostic_app(f411spi
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET f411spi)
  return() # diagnostics off -> app not built
endif()

# No CTest gate: this app answers a HARDWARE question (PMSA peripheral enforcement)
# and is validated by the operator on silicon, not by the in-env sim/QEMU.
