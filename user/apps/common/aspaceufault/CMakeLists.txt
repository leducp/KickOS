# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_ASPACE)
  return()
endif()

kickos_add_diagnostic_app(aspaceufault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET aspaceufault)
  return()
endif()

# 139 is KOS_EXIT_FAULT and "THREAD FAULT" the thread-kill banner. The gate asserts them as
# literals; computing them from the sources that produce them would assert nothing. The faulting
# read is the PROCESS's own, so fault isolation contains it and the record is the kill banner.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_aspace_ufault TARGET aspaceufault BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_aspace_ufault_rv64.sh"
    ARGS "THREAD FAULT" 139)
endif()
