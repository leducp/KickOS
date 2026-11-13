# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `budgetwit`: the budgets as the syscall ABI answers them.

if(NOT TARGET budgetwit)
  return()
endif()

# EXACTLY the arms main.cc registers. The four PAGES arms are a translating board's, and skip by
# name elsewhere.
set(_arms 21)
set(_skip "")
if(NOT KICKOS_HAVE_ASPACE)
  set(_arms 17)
  set(_skip
    "+skip - a run past the task's PAGES budget is refused: this board does not translate"
    "+skip - a run mapped and closed still charges its minter: this board does not translate"
    "+skip - a run a dead incarnation mapped elsewhere still charges its declaration: this board does not translate"
    "+skip - of two reservations racing a presync, one fits the PAGES budget: this board does not translate")
endif()
# A spawn past a budget is the budget's -KOS_EAGAIN (11), never an exhausted pool's -KOS_ENOMEM.
set(_absent "the hog's refused spawn rc=-12" "the sweeper's refused spawn rc=-12")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME budgetwit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh" "$<TARGET_FILE:budgetwit>"
            budgetwit ${_arms} ${_absent} ${_skip})
  kickos_boot_timeout(budgetwit "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh")
else()
  kickos_add_qemu_test(TARGET budgetwit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
    ARGS budgetwit ${_arms} ${_absent} ${_skip})
endif()
