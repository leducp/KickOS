# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Built only where the mechanism is compiled: with the knob off the app boots and honestly
# reports FAIL, which is not a fleet build failure to carry around.
if(NOT KICKOS_LIBC_REENT)
  return()
endif()

kickos_add_application(errnoprobe
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_errnoprobe TARGET errnoprobe
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_errnoprobe TARGET errnoprobe BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh")
endif()
