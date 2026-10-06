# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console reclaim witness, two images sharing one composition that keeps stdout the kernel's:
# the park arm (KICKOS_RW_MODE=0) and the drain arm (1).
if(KICKOS_CONSOLE STREQUAL "none" OR KICKOS_CONSOLE STREQUAL "rtt")
  message(STATUS
    "KickOS: reclaimwit skipped: KICKOS_CONSOLE=${KICKOS_CONSOLE} leaves the chip console "
    "path compiled out, so a console reclaim is not observable. Build it with "
    "-DKICKOS_CONSOLE=chip.")
  return()
endif()
foreach(_mode 0 1)
  set(_target reclaimwit)
  if(_mode EQUAL 1)
    set(_target reclaimwit_drain)
  endif()
  add_executable(${_target} main.cc)
  kickos_app_system(${_target} COMPOSITION system.yaml SYSTEM reclaimwit_system)
  kickos_emit_image(${_target})
  target_compile_definitions(${_target} PRIVATE KICKOS_RW_MODE=${_mode})
  if(KICKOS_CONSOLE STREQUAL "both")
    target_compile_definitions(${_target} PRIVATE KICKOS_RW_RTT=1)
  endif()
endforeach()
