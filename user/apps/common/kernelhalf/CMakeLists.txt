# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_ASPACE)
  return()
endif()

kickos_add_diagnostic_app(kernelhalf
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET kernelhalf)
  return()
endif()

# 139 is KOS_EXIT_FAULT and "THREAD FAULT" the thread-kill banner. The gate asserts them as
# literals; computing them from the sources that produce them would assert nothing. The access
# is an EL0 one, so armv8a contains it and the marker is the kill banner.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_ARM64_BOARDS)
  string(REPLACE "-" "_" _tag "${KICKOS_BOARD}")
  kickos_add_qemu_test(NAME ${_tag}_kernel_half TARGET kernelhalf BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_kernel_half.sh"
    ARGS "THREAD FAULT" 139)
endif()

# RV64 has a script of its own: `scause` carries no fault-status field, so a permission fault and
# a translation fault are the same number and the assertion that separates them cannot be ported.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_kernel_half TARGET kernelhalf BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_kernel_half_rv64.sh"
    ARGS "THREAD FAULT" 139)
endif()
