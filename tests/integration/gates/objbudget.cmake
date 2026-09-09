# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `objbudget`: the per-task object budget as the syscall ABI answers it.

if(NOT TARGET objbudget)
  return()
endif()

# EXACTLY the arms main.cc registers, all unconditional. Any slack lets an arm be deleted with
# every gate still green. Raise this with the arm count.
set(_arms 5)
# The refusal must be the BUDGET's. -KOS_ENOMEM (12) out of the ceiling create would mean the
# pool ran dry, which is the denial the budget exists to prevent, and the app prints the code
# it got before it judges it.
set(_absent "endpoint_create at the ceiling rc=-12")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME objbudget
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh" "$<TARGET_FILE:objbudget>"
            objbudget ${_arms} "${_absent}")
  set_tests_properties(objbudget PROPERTIES TIMEOUT 15)
elseif(KICKOS_CHIP STREQUAL "mps2" OR KICKOS_BOARD STREQUAL "microbit"
       OR KICKOS_BOARD STREQUAL "qemu-riscv" OR KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(TARGET objbudget
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
    ARGS objbudget ${_arms} "${_absent}")
endif()
