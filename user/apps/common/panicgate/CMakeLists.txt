# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Case 6 creates a task of its own.
foreach(_case 1 2 3 4 5 6)
  add_executable(panicgate${_case} main.cc)
  if(_case EQUAL 6)
    add_executable(panicgate_peer peer.cc)
    target_link_libraries(panicgate_peer PRIVATE kickos)
    kickos_app_system(panicgate6 COMPOSITION system.yaml)
  else()
    target_link_libraries(panicgate${_case} PRIVATE KickOS::kickos KickOS::system_default)
  endif()
  kickos_emit_image(panicgate${_case})
  target_compile_definitions(panicgate${_case} PRIVATE KICKOS_PANICGATE_CASE=${_case})
endforeach()
