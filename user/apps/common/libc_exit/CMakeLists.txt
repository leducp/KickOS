# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_LIBC)
  return()
endif()

kickos_add_application(libc_exit
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(NOT KICKOS_BUILD_TESTS)
  return()
endif()

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_libc_exit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_exit.sh" "$<TARGET_FILE:libc_exit>")
  set_tests_properties(sim_libc_exit PROPERTIES TIMEOUT 15)
endif()

if(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_libc_exit TARGET libc_exit BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_exit.sh")
endif()

if(KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_libc_exit TARGET libc_exit BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_exit.sh")
endif()

if(KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_libc_exit TARGET libc_exit BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_exit.sh")
endif()

if(KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_libc_exit TARGET libc_exit BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_exit.sh")
endif()
