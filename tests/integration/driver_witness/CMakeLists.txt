# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The driver witnesses' system (tests/integration/check_driver_restart.sh and
# check_driver_line.sh), from the composition KICKOS_WITNESS_SYSTEM against an installed KickOS
# package whose catalogue carries tests/drivers. KICKOS_WITNESS_ROOT_DEPTH adds
# composition_witness/root_depth.cc and root_depth_app.cc.

cmake_minimum_required(VERSION 3.24)
project(kickos_driver_witness LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

set(KICKOS_WITNESS_SYSTEM "" CACHE FILEPATH "The witness's composition")
option(KICKOS_WITNESS_ROOT_DEPTH "Fill and scan root's stack" OFF)
if(NOT EXISTS "${KICKOS_WITNESS_SYSTEM}")
  message(FATAL_ERROR "KICKOS_WITNESS_SYSTEM '${KICKOS_WITNESS_SYSTEM}' names no composition")
endif()

# The program the image runs as its main; the others are programs of their own, whichever the
# composition names.
set(KICKOS_WITNESS_MAIN "" CACHE STRING "The witness program that is the image's main")
set(_programs exits_client traps_client fails_client intask line_client watcher)
if(NOT KICKOS_WITNESS_MAIN IN_LIST _programs)
  message(FATAL_ERROR "KICKOS_WITNESS_MAIN is '${KICKOS_WITNESS_MAIN}', not one of ${_programs}")
endif()
set(_sources ${KICKOS_WITNESS_MAIN}.cc)
if(KICKOS_WITNESS_ROOT_DEPTH)
  list(APPEND _sources "${CMAKE_CURRENT_LIST_DIR}/../composition_witness/root_depth.cc"
                       "${CMAKE_CURRENT_LIST_DIR}/../composition_witness/root_depth_app.cc")
endif()
kickos_compose(witness_system "${KICKOS_WITNESS_SYSTEM}")
add_executable(driver_witness ${_sources})
target_link_libraries(driver_witness PRIVATE KickOS::kickos witness_system)
foreach(_program IN LISTS _programs)
  if(NOT _program STREQUAL KICKOS_WITNESS_MAIN)
    add_executable(${_program} ${_program}.cc)
    target_link_libraries(${_program} PRIVATE KickOS::kickos)
    kickos_program_link(${_program})
  endif()
endforeach()
if(KICKOS_WITNESS_ROOT_DEPTH)
  target_link_options(driver_witness PRIVATE "LINKER:--wrap=kos_notify_wait")
endif()
kickos_emit_image(driver_witness)
