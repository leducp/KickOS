# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The thread_local witness. Built only where the board compiles TLS: elsewhere the
# thread_local would be a link error naming __aeabi_read_tp, which is the intended
# diagnostic but not something to fail the fleet build on.
if(NOT KICKOS_TLS)
  return()
endif()

kickos_add_application(tlsprobe
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_tlsprobe TARGET tlsprobe
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_tlsprobe.sh")
endif()

# The armv6m arm. Without it the leaf is compiled and structurally gated on picopi but run
# nowhere, and armv6m is the half of the ARM fleet the armv7m witness does not speak for.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_tlsprobe TARGET tlsprobe BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_tlsprobe.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv" AND NOT KICKOS_HAVE_MPU)
  kickos_add_qemu_test(NAME qemu_riscv_tlsprobe TARGET tlsprobe BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_tlsprobe.sh")
endif()
