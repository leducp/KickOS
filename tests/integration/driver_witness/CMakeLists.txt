# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The driver restart witness's system (tests/integration/check_driver_restart.sh), from the
# composition KICKOS_WITNESS_SYSTEM against an installed KickOS package whose catalogue carries
# tests/drivers. KICKOS_WITNESS_ROOT_DEPTH adds composition_witness/root_depth.cc.

cmake_minimum_required(VERSION 3.24)
project(kickos_driver_witness LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

set(KICKOS_WITNESS_SYSTEM "" CACHE FILEPATH "The witness's composition")
option(KICKOS_WITNESS_ROOT_DEPTH "Fill and scan root's stack" OFF)
if(NOT EXISTS "${KICKOS_WITNESS_SYSTEM}")
  message(FATAL_ERROR "KICKOS_WITNESS_SYSTEM '${KICKOS_WITNESS_SYSTEM}' names no composition")
endif()

set(_sources clients.cc watcher.cc)
if(KICKOS_WITNESS_ROOT_DEPTH)
  list(APPEND _sources "${CMAKE_CURRENT_LIST_DIR}/../composition_witness/root_depth.cc")
endif()
kickos_compose(witness_system "${KICKOS_WITNESS_SYSTEM}")
add_executable(driver_witness ${_sources})
target_link_libraries(driver_witness PRIVATE KickOS::kernel witness_system)
if(KICKOS_WITNESS_ROOT_DEPTH)
  target_link_options(driver_witness PRIVATE "LINKER:--wrap=kos_notify_wait")
endif()
kickos_emit_image(driver_witness)
