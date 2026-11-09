# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ESP32 cache MMU entries that map task text from flash (arch/xtensa/chip/esp32/flash_mmu.h),
# against the TRM's entry tables.
kickos_add_unit_test(NAME flash_mmu
  SOURCES flash_mmu.cc
  INCLUDES "${PROJECT_SOURCE_DIR}")
