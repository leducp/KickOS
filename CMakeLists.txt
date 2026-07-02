# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
project(KickOS VERSION 0.0.1 LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/kickos.cmake)

# --- Board / arch selection -------------------------------------------------
set(KICKOS_BOARD "sim" CACHE STRING "Target board: sim|frdmk64f|picopi|f411disco|bluepill")
kickos_resolve_board("${KICKOS_BOARD}" KICKOS_ARCH)
message(STATUS "KickOS: board=${KICKOS_BOARD} arch=${KICKOS_ARCH}")

# --- Config knobs (architecture.md) ----------------------------------------
option(CONFIG_SCHED_PERIODIC_TICK "Force a classic periodic scheduler tick" OFF)
option(KICKOS_BUILD_APPS          "Build in-tree demo applications"          ON)
option(KICKOS_BUILD_TESTS         "Register CTest cases"                     ON)

# Public include roots shared by the component libraries.
set(KICKOS_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/arch/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/user/include")

if(KICKOS_BUILD_TESTS)
  enable_testing()
endif()

# Components are added as they land; guarded so every step boundary configures.
foreach(_dir lib arch kernel user)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
    add_subdirectory(${_dir})
  endif()
endforeach()

if(KICKOS_BUILD_APPS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user/apps/CMakeLists.txt")
  add_subdirectory(user/apps)
endif()
