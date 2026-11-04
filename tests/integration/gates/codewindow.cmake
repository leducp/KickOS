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
