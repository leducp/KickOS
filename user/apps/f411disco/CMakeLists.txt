# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# f411disco-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# f411disco), and the diagnostic posture is applied here so the app files never ask.

# STM32F411 SPI1 loopback driver: the canonical PMSA per-thread MMIO isolation reference. The
# MMIO-grant seam exists only under enforcement.
if(KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(f411spi)
endif()
