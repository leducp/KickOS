# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Each chip port's console baud divisor, rate and baud in, register value out, at every rate
# the port can land on. The chips' own headers, over planted bases (divisor_mmap.h).
kickos_add_unit_test(NAME console_baud
  SOURCES console_baud.cc
  INCLUDES "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/generated/include")
target_compile_options(console_baud PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/divisor_mmap.h")
