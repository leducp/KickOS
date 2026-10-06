# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The default system's witnesses: a plain C main returning 3, the same while a thread it created
# spins, and a main that faults.
foreach(_app sysdefault sysdefault_spin sysdefault_fault)
  set(_source main.c)
  if(_app STREQUAL "sysdefault_spin")
    set(_source spin.c)
  elseif(_app STREQUAL "sysdefault_fault")
    set(_source fault.c)
  endif()
  add_executable(${_app} ${_source})
  target_link_libraries(${_app} PRIVATE KickOS::kernel KickOS::system_default)
  kickos_emit_image(${_app})
endforeach()
