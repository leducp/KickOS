# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_LIBC)
  return()
endif()

kickos_add_diagnostic_app(rootfault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET rootfault)
  return()
endif()

# KICKOS_MEMORY_ENFORCED, the arm being about CONFINEMENT and not about which mechanism
# enforces it. The script reads the fault address off whichever field the reporter prints.
if(KICKOS_BUILD_TESTS AND KICKOS_MEMORY_ENFORCED)
  # Through the same script as the QEMU boards: a PASS/FAIL_REGULAR_EXPRESSION pair cannot
  # require the child's control marker AND pin the fault address to the region root announced.
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME rootfault
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_rootfault.sh" "$<TARGET_FILE:rootfault>"
              ${KICKOS_FAULT_OUTCOME})
    set_tests_properties(rootfault PROPERTIES TIMEOUT 15)
  elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_rootfault TARGET rootfault
      BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootfault.sh" ARGS ${KICKOS_FAULT_OUTCOME})
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv")
    kickos_add_qemu_test(NAME qemu_riscv_rootfault TARGET rootfault BOARD qemu-riscv
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootfault.sh" ARGS ${KICKOS_FAULT_OUTCOME})
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv64")
    kickos_add_qemu_test(NAME qemu_riscv64_rootfault TARGET rootfault BOARD qemu-riscv64
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootfault.sh" ARGS ${KICKOS_FAULT_OUTCOME})
  endif()
endif()
