# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The privilege windows: a debugger preempts a thread inside a kernel window and rewrites its
# stacked PC wherever the frame lies in the thread's own stack, or pends the switch alone.

if(NOT TARGET privwindow_svc)
  return()
endif()
find_program(KICKOS_GDB NAMES gdb-multiarch)
set(_pw_gdb gdb-multiarch)
if(KICKOS_GDB)
  set(_pw_gdb "${KICKOS_GDB}")
endif()
set(_pw_gate "${PROJECT_SOURCE_DIR}/tests/integration/check_privwindow.sh")
set(_pw_images privwindow_svc privwindow_fault privwindow_callreg privwindow_resume)
foreach(_pw_mode 0 1 2 3)
  list(GET _pw_images ${_pw_mode} _pw_image)
  kickos_emulator_judged(${_pw_image})
  kickos_add_qemu_test(TARGET ${_pw_image}
    SCRIPT "${_pw_gate}" ARGS ${_pw_mode} "${_pw_gdb}" "${CMAKE_NM}")
endforeach()
