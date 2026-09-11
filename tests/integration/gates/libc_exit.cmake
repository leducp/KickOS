# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The libc exit-path gate riding `libc_exit`, through one script on the host and on every
# emulated board.

if(NOT TARGET libc_exit)
  return()
endif()

set(_libc_exit_script "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_exit.sh")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_libc_exit
    COMMAND "${_libc_exit_script}" "$<TARGET_FILE:libc_exit>")
  set_tests_properties(sim_libc_exit PROPERTIES TIMEOUT 15)
endif()

# Not armv8a and not x86_64: those build the app and register no arm today.
if(NOT KICKOS_ARCH STREQUAL "armv8a" AND NOT KICKOS_ARCH STREQUAL "x86_64")
  kickos_add_qemu_test(TARGET libc_exit SCRIPT "${_libc_exit_script}")
endif()
