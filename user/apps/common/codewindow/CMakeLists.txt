# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The RV32 PMP code window: the first and last words read, and a fetch from the word below it
# or the word past it faults.
foreach(_probe below past)
  string(TOUPPER "${_probe}" _define)
  add_executable(codewindow_${_probe} main.cc)
  target_compile_definitions(codewindow_${_probe} PRIVATE CODEWINDOW_${_define}=1)
  target_link_libraries(codewindow_${_probe} PRIVATE KickOS::kickos KickOS::system_default)
  kickos_emit_image(codewindow_${_probe})
endforeach()
