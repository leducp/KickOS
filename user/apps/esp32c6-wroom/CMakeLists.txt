# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# esp32c6-wroom-only apps. The directory is the board gate (added only when
# KICKOS_BOARD is esp32c6-wroom); each app keeps its own selftest gate.

# ESP32-C6 GPIO10 blink -- the RISC-V PMP per-thread MMIO isolation reference. The
# MMIO-grant seam is an M2 construct, so enforcement builds only; also self-gates on
# KICKOS_ENABLE_SELFTEST.
if(KICKOS_HAVE_MPU)
  add_subdirectory(c6blink)
endif()

# CPU-interrupt-controller identity probe (INTPRI @0x600C_5000 vs the undocumented
# 0x2000_1000 window). Needs the enforcement grant predicates, so enforcement builds only;
# also self-gates on KICKOS_ENABLE_SELFTEST.
if(KICKOS_HAVE_MPU)
  add_subdirectory(c6intpri)
endif()
