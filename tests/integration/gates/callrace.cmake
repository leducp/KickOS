# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The SVC fastpath against a device interrupt inside its trap, riding `callrace`: the emulator's
# debugger pends the waker's line there.

if(NOT TARGET callrace)
  return()
endif()
kickos_emulator_judged(callrace)
find_program(KICKOS_GDB NAMES gdb-multiarch)
set(_cr_gdb gdb-multiarch)
if(KICKOS_GDB)
  set(_cr_gdb "${KICKOS_GDB}")
endif()
kickos_add_qemu_test(TARGET callrace
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_callrace.sh"
  ARGS "${_cr_gdb}" "${CMAKE_NM}" BOUND 30)
