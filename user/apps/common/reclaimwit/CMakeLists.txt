# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(KICKOS_CONSOLE STREQUAL "none" OR KICKOS_CONSOLE STREQUAL "rtt")
  message(STATUS
    "KickOS: reclaimwit skipped: KICKOS_CONSOLE=${KICKOS_CONSOLE} leaves the chip console "
    "path compiled out, so a console reclaim is not observable. Build it with "
    "-DKICKOS_CONSOLE=chip.")
  return()
endif()

if(NOT KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  message(STATUS
    "KickOS: reclaimwit built, but KICKOS_SERVICE_LIST=${KICKOS_SERVICE_LIST} may already "
    "publish a console; the app refuses at run time if it does. It needs "
    "-DKICKOS_SERVICE_LIST=kickos_services_none to give a verdict.")
endif()

kickos_add_app_target(reclaimwit main.cc)
target_link_libraries(reclaimwit PRIVATE kickos)
kickos_emit_image(reclaimwit)
target_compile_definitions(reclaimwit PRIVATE KICKOS_RW_MODE=0)

# Its own binary, it ends the system, and built alongside the park arm so both #if branches of
# main.cc stay compiled.
kickos_add_app_target(reclaimwit_drain main.cc)
target_link_libraries(reclaimwit_drain PRIVATE kickos)
kickos_emit_image(reclaimwit_drain)
target_compile_definitions(reclaimwit_drain PRIVATE KICKOS_RW_MODE=1)

# With `both`, RTT carries kernel writes in every ownership state, so the MUTE line reaches
# an RTT viewer even on a correct run.
if(KICKOS_CONSOLE STREQUAL "both")
  target_compile_definitions(reclaimwit PRIVATE KICKOS_RW_RTT=1)
  target_compile_definitions(reclaimwit_drain PRIVATE KICKOS_RW_RTT=1)
endif()
