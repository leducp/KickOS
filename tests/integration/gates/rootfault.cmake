# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `rootfault`: the default composition's main steps into the region of a task it
# created and the child says so.

if(NOT TARGET rootfault)
  return()
endif()
if(NOT KICKOS_MEMORY_ENFORCED)
  kickos_inapplicable(rootfault "memory not enforced")
endif()

# KICKOS_MEMORY_ENFORCED, the arm being about CONFINEMENT and not about which mechanism
# enforces it. The script reads the fault address off whichever field the reporter prints.
if(KICKOS_MEMORY_ENFORCED)
  kickos_app_judge(rootfault tests/integration/check_rootfault.sh ARGS ${KICKOS_FAULT_OUTCOME})
  # Through the same script as the QEMU boards: a PASS/FAIL_REGULAR_EXPRESSION pair cannot
  # require the child's control marker AND pin the fault address to the region main announced.
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME rootfault
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_rootfault.sh" "$<TARGET_FILE:rootfault>"
              ${KICKOS_FAULT_OUTCOME})
    set_tests_properties(rootfault PROPERTIES TIMEOUT 40)
  else()
    kickos_add_qemu_test(TARGET rootfault
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_rootfault.sh"
      ARGS ${KICKOS_FAULT_OUTCOME})
  endif()
endif()
