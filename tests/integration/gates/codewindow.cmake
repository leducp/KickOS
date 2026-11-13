# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `codewindow_below` and `codewindow_past`: the RV32 PMP code window ends where
# the code ends.

if(NOT TARGET codewindow_below)
  return()
endif()
foreach(_probe below past)
  kickos_app_judge(codewindow_${_probe} tests/integration/check_codewindow.sh ARGS ${_probe})
  kickos_add_qemu_test(NAME ${_tag}_codewindow_${_probe} TARGET codewindow_${_probe}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_codewindow.sh"
    ARGS ${_probe})
endforeach()
# A boot stage's PMP entry 12, planted through the emulator's gdb stub.
find_program(KICKOS_GDB NAMES gdb-multiarch)
set(_cw_gdb gdb-multiarch)
if(KICKOS_GDB)
  set(_cw_gdb "${KICKOS_GDB}")
endif()
foreach(_plant stale locked)
  kickos_add_qemu_test(NAME ${_tag}_codewindow_past_${_plant} TARGET codewindow_past
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_codewindow.sh"
    ARGS past ${_plant} "${_cw_gdb}" BOUND 60)
endforeach()
