# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `rootauth`: the authority arms of a task declaring [memory, system, pinmux] in
# its composition, counted.

if(NOT TARGET rootauth)
  return()
endif()

# EXACTLY the arms main.cc registers, all unconditional. Any slack lets an arm be deleted with
# every gate still green. Raise this with the arm count.
set(_arms 5)

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME rootauth
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh" "$<TARGET_FILE:rootauth>"
            rootauth ${_arms})
  set_tests_properties(rootauth PROPERTIES TIMEOUT 15)
# Not armv8a and not x86_64: those build the app and register no arm today.
elseif(NOT KICKOS_ARCH STREQUAL "armv8a" AND NOT KICKOS_ARCH STREQUAL "x86_64")
  kickos_add_qemu_test(TARGET rootauth
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
    ARGS rootauth ${_arms})
endif()
