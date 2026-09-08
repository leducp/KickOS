# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `rootgone`: root dies of its own violation and the system carries on.

if(NOT TARGET rootgone)
  return()
endif()

# The arm needs root's death to be CONTAINED rather than fatal: on the panic posture root's
# violation ends the system, and there is no surviving thread to ask the question of. So this
# one is narrower than rootfault's registration, which covers both outcomes.
if(KICKOS_MEMORY_ENFORCED AND KICKOS_FAULT_OUTCOME STREQUAL "thread-kill")
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME rootgone
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_rootgone.sh" "$<TARGET_FILE:rootgone>")
    set_tests_properties(rootgone PROPERTIES TIMEOUT 30)
  # qemu-arm64 is ALSO the SM-6 posture: KICKOS_HAVE_ASPACE makes root's stack a mapped run the
  # kernel frees at its death, where every other board here keeps it an arena block.
  elseif(KICKOS_CHIP STREQUAL "mps2" OR KICKOS_BOARD STREQUAL "qemu-riscv"
         OR KICKOS_BOARD STREQUAL "qemu-riscv64" OR KICKOS_BOARD STREQUAL "qemu-arm64")
    kickos_add_qemu_test(TARGET rootgone
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootgone.sh")
  endif()
endif()
