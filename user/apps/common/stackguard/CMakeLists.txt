# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_ASPACE)
  return()
endif()

kickos_add_diagnostic_app(stackguard
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET stackguard)
  return()
endif()

# 139 is KOS_EXIT_FAULT and "THREAD FAULT" the thread-kill banner. The gate asserts them as
# literals; computing them from the sources that produce them would assert nothing. The access
# is an EL0 one, so armv8a contains it and the marker is the kill banner.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_stack_guard TARGET stackguard BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stack_guard.sh"
    ARGS "THREAD FAULT" 139)
endif()

# RV64 has a script of its own: the write bit lives in the CAUSE here, so there is no constant
# to substitute.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_stack_guard TARGET stackguard BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stack_guard_rv64.sh"
    ARGS "THREAD FAULT" 139)
endif()
