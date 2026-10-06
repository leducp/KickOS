# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ESP32-C6 partition gate's register image (arch/riscv/chip/esp32c6/apm_rows.h), programmed
# over a fake APM register file. c6_mmap.h is forced ahead of every include: a host build
# generates no ESP32-C6 chip header.
kickos_add_unit_test(NAME c6apm
  SOURCES c6apm.cc
  INCLUDES "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/user/include")
target_compile_options(c6apm PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/c6_mmap.h")
