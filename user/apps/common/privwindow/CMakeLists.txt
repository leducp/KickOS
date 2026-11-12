# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A thread's context caught between a privileged instruction and the memory it rests in. The
# gate's debugger preempts the thread at the window and, standing in for a task-mate, rewrites
# the stacked PC wherever the frame lies in the thread's own stack. One image per window.
function(privwindow_image target mode)
  add_executable(${target} main.cc)
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
  target_compile_definitions(${target} PRIVATE KICKOS_PRIVWINDOW_MODE=${mode})
  target_link_libraries(${target} PRIVATE KickOS::kickos KickOS::system_default)
  kickos_emit_image(${target})
endfunction()

privwindow_image(privwindow_svc 0)
privwindow_image(privwindow_fault 1)
privwindow_image(privwindow_callreg 2)
privwindow_image(privwindow_resume 3)
