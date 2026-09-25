# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The libc reentrancy-seat gate riding `errnoprobe`. The board predicates are load-bearing:
# microbit builds the app and registers no arm.

if(NOT TARGET errnoprobe)
  return()
endif()

set(_errnoprobe_script "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh")

if(KICKOS_CHIP STREQUAL "mps2" OR KICKOS_BOARD STREQUAL "qemu-riscv"
   OR KICKOS_BOARD STREQUAL "qemu-riscv64" OR KICKOS_ARCH STREQUAL "armv8a")
  kickos_add_qemu_test(TARGET errnoprobe SCRIPT "${_errnoprobe_script}")
endif()
