# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The libc exit-path gate riding `libc_exit` and `libc_exit_abort`, through one script on the
# host and on every emulated board.

if(NOT TARGET libc_exit)
  return()
endif()
set(_libc_exit_args --atexit)
kickos_app_judge(libc_exit tests/integration/check_libc_exit.sh ARGS ${_libc_exit_args})

set(_libc_exit_script "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_exit.sh")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_libc_exit
    COMMAND "${_libc_exit_script}" "$<TARGET_FILE:libc_exit>")
  set_tests_properties(sim_libc_exit PROPERTIES TIMEOUT 15)
endif()

# Every other emulated board runs newlib's own exit(), which runs the task's handlers.
kickos_add_qemu_test(TARGET libc_exit SCRIPT "${_libc_exit_script}" ARGS ${_libc_exit_args})

if(TARGET libc_exit_abort)
  kickos_app_judge(libc_exit_abort tests/integration/check_libc_exit.sh ARGS --abort)
  kickos_add_qemu_test(TARGET libc_exit_abort SCRIPT "${_libc_exit_script}" ARGS --abort)
endif()
