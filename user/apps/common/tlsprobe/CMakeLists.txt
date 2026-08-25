# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Built only where the board compiles TLS: elsewhere the thread_local is a link error naming
# __aeabi_read_tp, which is the intended diagnostic but not a fleet build failure.
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

# The armv6m arm, which the armv7m witness above does not speak for.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_tlsprobe TARGET tlsprobe BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_tlsprobe.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_tlsprobe TARGET tlsprobe BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_tlsprobe.sh")
endif()

# The only arch in the fleet that SEATS the thread pointer instead of masking SP down to a
# stride, so this arm is what says the seating is real rather than the mask by another name.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_tlsprobe TARGET tlsprobe BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_tlsprobe.sh")
endif()

# The RX emutls block is proved offline: rxv3 has no QEMU machine, so exhausting it is a
# first-touch PANIC nobody can run. Every input the bump allocator uses is a link-time
# constant, so the allocation is replayed against the linked image instead. GAP: this is the
# only RX image in the tree that declares a thread_local, so a consumer app that declares more
# is not covered.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "rxv3")
  add_test(
    NAME    rx_tls_fit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/rx_tls_fit.py"
            "${CMAKE_NM}" "${CMAKE_OBJDUMP}" "${CMAKE_BINARY_DIR}/user/apps")
  set_tests_properties(rx_tls_fit PROPERTIES TIMEOUT 60 LABELS host)
endif()
