# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_LIBC)
  return()
endif()

kickos_add_diagnostic_app(rootgone
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET rootgone)
  return()
endif()

# The arm needs root's death to be CONTAINED rather than fatal: on the panic posture root's
# violation ends the system, and there is no surviving thread to ask the question of. So this
# one is narrower than rootfault's registration, which covers both outcomes.
if(KICKOS_BUILD_TESTS AND KICKOS_MEMORY_ENFORCED
   AND KICKOS_FAULT_OUTCOME STREQUAL "thread-kill")
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME rootgone
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_rootgone.sh" "$<TARGET_FILE:rootgone>")
    set_tests_properties(rootgone PROPERTIES TIMEOUT 30)
  elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_rootgone TARGET rootgone
      BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootgone.sh")
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv")
    kickos_add_qemu_test(NAME qemu_riscv_rootgone TARGET rootgone BOARD qemu-riscv
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootgone.sh")
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv64")
    kickos_add_qemu_test(NAME qemu_riscv64_rootgone TARGET rootgone BOARD qemu-riscv64
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootgone.sh")
  elseif(KICKOS_BOARD STREQUAL "qemu-arm64")
    # ALSO the SM-6 posture: KICKOS_HAVE_ASPACE makes root's stack a mapped run the kernel
    # frees at its death, where every board above keeps it an arena block.
    kickos_add_qemu_test(NAME qemu_arm64_rootgone TARGET rootgone BOARD qemu-arm64
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootgone.sh")
  endif()
endif()
