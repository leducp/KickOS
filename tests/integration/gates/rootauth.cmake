# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `rootauth`: the authority arms root holds, counted.

if(NOT TARGET rootauth)
  return()
endif()

# EXACTLY the arms main.cc registers, all unconditional. Any slack lets an arm be deleted with
# every gate still green. Raise this with the arm count.
set(_arms 5)
# main prints PASS and then returns, and that return calls kos_shutdown (gated on
# KOS_AUTH_SYSTEM), so a regression dropping only that bit panics AFTER the verdict.
set(_absent "root: shutdown refused")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME rootauth
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh" "$<TARGET_FILE:rootauth>"
            rootauth ${_arms} "${_absent}")
  set_tests_properties(rootauth PROPERTIES TIMEOUT 15)
elseif(KICKOS_CHIP STREQUAL "mps2" OR KICKOS_BOARD STREQUAL "microbit"
       OR KICKOS_BOARD STREQUAL "qemu-riscv" OR KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(TARGET rootauth
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
    ARGS rootauth ${_arms} "${_absent}")
endif()
