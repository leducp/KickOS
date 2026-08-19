# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# f411disco-only apps. The directory is the board gate (added only when KICKOS_BOARD
# is f411disco); each app keeps its own selftest gate.

# STM32F411 SPI1 loopback driver: the canonical PMSA per-thread MMIO isolation
# reference. The MMIO-grant seam is an M2 construct, so enforcement builds only; also
# self-gates on KICKOS_ENABLE_SELFTEST.
if(KICKOS_HAVE_MPU)
  add_subdirectory(f411spi)
endif()
