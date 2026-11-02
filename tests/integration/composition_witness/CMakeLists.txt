# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The systems behind tests/integration/check_composition_witness.sh, each built against an
# installed KickOS package as examples/composition is, from the composition KICKOS_WITNESS_SYSTEM
# names, its witness KICKOS_WITNESS:
#   restart  app.cc and the programs health (health.cc) and restart_sensor (sensor.cc), the
#            golden example's app and health checker printing self-delimiting lines, with the
#            test sensor, each task checking own.cc's word is its own;
#            with KICKOS_WITNESS_ROOT_DEPTH, also root_depth.cc's fill and scan of root's stack,
#            and root_depth_app.cc's plain wait for the app's image;
#   line     the line taker of alarm.cc;
#   priority the init's priority witness: priority_low.cc, and the programs priority_high and
#            priority_pulse.

cmake_minimum_required(VERSION 3.24)
project(kickos_composition_witness LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

set(KICKOS_WITNESS "" CACHE STRING "The witness to build: restart, line or priority")
set(KICKOS_WITNESS_SYSTEM "" CACHE FILEPATH "The witness's composition")
option(KICKOS_WITNESS_ROOT_DEPTH "Fill and scan root's stack in the restart witness" OFF)

# sample.h, the example's message and history formats.
set(_example "${CMAKE_CURRENT_LIST_DIR}/../../../examples/composition")
# The sources of the image's main, and the programs of their own, each a target of its own name
# the composition may name.
set(_programs "")
if(KICKOS_WITNESS STREQUAL "restart")
  set(_sources app.cc own.cc)
  set(_programs health restart_sensor)
  set(_health_sources health.cc own.cc)
  set(_restart_sensor_sources sensor.cc own.cc)
  if(KICKOS_WITNESS_ROOT_DEPTH)
    list(APPEND _sources root_depth.cc root_depth_app.cc)
  endif()
elseif(KICKOS_WITNESS STREQUAL "line")
  set(_sources alarm.cc)
elseif(KICKOS_WITNESS STREQUAL "priority")
  set(_sources priority_low.cc)
  set(_programs priority_high priority_pulse)
  set(_priority_high_sources priority_high.cc)
  set(_priority_pulse_sources priority_pulse.cc)
else()
  message(FATAL_ERROR "KICKOS_WITNESS is '${KICKOS_WITNESS}', not restart, line or priority")
endif()
if(NOT EXISTS "${KICKOS_WITNESS_SYSTEM}")
  message(FATAL_ERROR "KICKOS_WITNESS_SYSTEM '${KICKOS_WITNESS_SYSTEM}' names no composition")
endif()
kickos_compose(witness_system "${KICKOS_WITNESS_SYSTEM}")

add_executable(witness ${_sources})
target_include_directories(witness PRIVATE "${_example}")
target_link_libraries(witness PRIVATE KickOS::kickos witness_system)
foreach(_program IN LISTS _programs)
  add_executable(${_program} ${_${_program}_sources})
  target_include_directories(${_program} PRIVATE "${_example}")
  target_link_libraries(${_program} PRIVATE KickOS::kickos)
  kickos_program_link(${_program})
endforeach()
if(KICKOS_WITNESS_ROOT_DEPTH)
  target_link_options(witness PRIVATE "LINKER:--wrap=kos_notify_wait")
endif()
kickos_emit_image(witness)
