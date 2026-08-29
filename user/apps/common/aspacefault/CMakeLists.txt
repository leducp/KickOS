# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_ASPACE)
  return()
endif()

kickos_add_diagnostic_app(aspacefault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET aspacefault)
  return()
endif()

# 132 is kfault_terminate's status and "ARMV8A EXCEPTION" this arch's panic banner. The gate
# asserts them as literals; computing them from the sources that produce them would assert
# nothing. The faulting read is the KERNEL's own, inside op_touch_unmapped, so it arrives at the
# current-EL vector and the record is the panic banner.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_aspace_fault TARGET aspacefault BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_aspace_fault.sh"
    ARGS "ARMV8A EXCEPTION" 132)
endif()
