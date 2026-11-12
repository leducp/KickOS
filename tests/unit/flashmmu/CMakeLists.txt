# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ESP32 cache MMU entries that map task text from flash (arch/xtensa/chip/esp32/flash_mmu.h),
# against the TRM's entry tables.
kickos_add_unit_test(NAME flash_mmu
  SOURCES flash_mmu.cc
  INCLUDES "${PROJECT_SOURCE_DIR}")

# The ESP32-C6 cache MMU entries that map task text from flash
# (arch/riscv/chip/esp32c6/flash_mmu.h).
kickos_add_unit_test(NAME flash_mmu_c6
  SOURCES flash_mmu_c6.cc
  INCLUDES "${PROJECT_SOURCE_DIR}")
