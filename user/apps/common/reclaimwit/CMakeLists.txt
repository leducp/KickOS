# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Board-agnostic console-reclaim and terminate-drain witness.
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

kickos_add_diagnostic_app(reclaimwit
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET reclaimwit)
  return() # diagnostics off -> app not built
endif()
target_compile_definitions(reclaimwit PRIVATE KICKOS_RW_MODE=0)

# A separate binary because it ENDS the system. Always built alongside the park arm, so
# both #if branches of main.cc stay compiled.
kickos_add_diagnostic_app(reclaimwit_drain
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
target_compile_definitions(reclaimwit_drain PRIVATE KICKOS_RW_MODE=1)

# With `both`, RTT carries kernel writes in every ownership state, so the MUTE line reaches
# an RTT viewer even on a correct run.
if(KICKOS_CONSOLE STREQUAL "both")
  target_compile_definitions(reclaimwit PRIVATE KICKOS_RW_RTT=1)
  target_compile_definitions(reclaimwit_drain PRIVATE KICKOS_RW_RTT=1)
endif()

# No CTest gate: PASS is an operator reading a silicon wire.
