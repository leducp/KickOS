# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The systems behind tests/integration/check_composition_witness.sh, each built against an
# installed KickOS package as examples/composition is, from the composition KICKOS_WITNESS_SYSTEM
# names, its witness KICKOS_WITNESS:
#   restart  app.cc and health.cc, the golden example's app and health checker printing
#            self-delimiting lines, with the test sensor of sensor.cc;
#            with KICKOS_WITNESS_ROOT_DEPTH, also root_depth.cc's fill and scan of root's stack;
#   line     the line taker of alarm.cc;
#   priority the init's priority witness of priority.cc.

cmake_minimum_required(VERSION 3.24)
project(kickos_composition_witness LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

set(KICKOS_WITNESS "" CACHE STRING "The witness to build: restart, line or priority")
set(KICKOS_WITNESS_SYSTEM "" CACHE FILEPATH "The witness's composition")
option(KICKOS_WITNESS_ROOT_DEPTH "Fill and scan root's stack in the restart witness" OFF)

# sample.h, the example's message and history formats.
set(_example "${CMAKE_CURRENT_LIST_DIR}/../../../examples/composition")
if(KICKOS_WITNESS STREQUAL "restart")
  set(_sources app.cc health.cc sensor.cc)
  if(KICKOS_WITNESS_ROOT_DEPTH)
    list(APPEND _sources root_depth.cc)
  endif()
elseif(KICKOS_WITNESS STREQUAL "line")
  set(_sources alarm.cc)
elseif(KICKOS_WITNESS STREQUAL "priority")
  set(_sources priority.cc)
else()
  message(FATAL_ERROR "KICKOS_WITNESS is '${KICKOS_WITNESS}', not restart, line or priority")
endif()
if(NOT EXISTS "${KICKOS_WITNESS_SYSTEM}")
  message(FATAL_ERROR "KICKOS_WITNESS_SYSTEM '${KICKOS_WITNESS_SYSTEM}' names no composition")
endif()
kickos_compose(witness_system "${KICKOS_WITNESS_SYSTEM}")

if(KICKOS_ARCH STREQUAL "x86_64")
  kickos_add_app_target(witness ${_sources})
else()
  add_executable(witness ${_sources})
endif()
target_include_directories(witness PRIVATE "${_example}")
target_link_libraries(witness PRIVATE KickOS::kernel witness_system)
if(KICKOS_WITNESS_ROOT_DEPTH)
  target_link_options(witness PRIVATE "LINKER:--wrap=kos_notify_wait")
endif()
kickos_emit_image(witness)
