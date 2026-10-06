# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `rootfault`: the default composition's main steps into the region of a task it
# created and the child says so.

if(NOT TARGET rootfault)
  return()
endif()

# KICKOS_MEMORY_ENFORCED, the arm being about CONFINEMENT and not about which mechanism
# enforces it. The script reads the fault address off whichever field the reporter prints.
if(KICKOS_MEMORY_ENFORCED)
  # Through the same script as the QEMU boards: a PASS/FAIL_REGULAR_EXPRESSION pair cannot
  # require the child's control marker AND pin the fault address to the region main announced.
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME rootfault
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_rootfault.sh" "$<TARGET_FILE:rootfault>"
              ${KICKOS_FAULT_OUTCOME})
    set_tests_properties(rootfault PROPERTIES TIMEOUT 40)
  elseif(KICKOS_QEMU_MPS2 OR KICKOS_BOARD STREQUAL "qemu-riscv"
         OR KICKOS_BOARD STREQUAL "qemu-riscv64")
    kickos_add_qemu_test(TARGET rootfault
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootfault.sh"
      ARGS ${KICKOS_FAULT_OUTCOME})
  endif()
endif()
