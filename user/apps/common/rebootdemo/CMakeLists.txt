# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_LIBC)
  return()
endif()

kickos_add_diagnostic_app(rebootdemo
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET rebootdemo)
  return()
endif()

if(NOT KICKOS_BUILD_TESTS)
  return()
endif()

# rc=-38 is -KOS_ENOSYS (system/include/kickos/sys/errno.h).
if(KICKOS_ARCH STREQUAL "sim")
  # The script, not PASS_REGULAR_EXPRESSION: a regex property makes CTest ignore the exit
  # status, so the refusal line plus a dirty exit would still pass.
  add_test(NAME sim_reboot_declined
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_reboot.sh" "$<TARGET_FILE:rebootdemo>")
  set_tests_properties(sim_reboot_declined PROPERTIES TIMEOUT 20)
elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_reboot_declined TARGET rebootdemo
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_reboot.sh" TIMEOUT 40)
elseif(KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_reboot_declined TARGET rebootdemo BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_reboot.sh" TIMEOUT 40)
elseif(KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_reboot_declined TARGET rebootdemo BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_reboot.sh" TIMEOUT 40)
endif()
