# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# esp32c6-wroom-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# esp32c6-wroom), and the diagnostic posture is applied here so the app files never ask.

# c6blink is the RISC-V PMP per-thread MMIO isolation reference and c6intpri the
# CPU-interrupt-controller identity probe (INTPRI @0x600C_5000 vs the undocumented
# 0x2000_1000 window). Both need the MMIO-grant seam and the enforcement grant predicates,
# which exist only under enforcement.
if(KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(c6blink c6intpri)
endif()
