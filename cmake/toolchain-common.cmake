# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The part every KickOS cross toolchain file spells the same way.
#
# MACROS, not functions: these set variables CMake and the including file read afterwards,
# which a function's own scope would not export.
#
# CMAKE_CURRENT_LIST_DIR inside a macro body is the INCLUDING file's directory, so the relative
# paths below resolve against the toolchain file: cmake/ in tree, lib/cmake/KickOS/ in an
# installed package.

# kickos_toolchain_board_descriptor(<label>)
#
# The caller must set KICKOS_TOOLCHAIN_DEFAULT_BOARD first: toolchain-package-board.cmake
# refuses a configure without it.
macro(kickos_toolchain_board_descriptor _kos_tc_label)
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake")
  elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/board.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/board.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/toolchain-package-board.cmake")
  else()
    message(FATAL_ERROR
      "KickOS ${_kos_tc_label} toolchain: no board descriptor for '${KICKOS_BOARD}'")
  endif()
endmacro()

# kickos_toolchain_cpu_baseline(<label> <arch-dir>)
#
# Sets _kos_cpu in the caller. Included AFTER the board descriptor, so a board that states its
# own core or float ABI wins. An installed package has no arch/ tree and ships a descriptor
# with the flags resolved into it: a missing cpu.cmake is not an error, a missing value is.
macro(kickos_toolchain_cpu_baseline _kos_tc_label _kos_tc_arch)
  set(_kos_tc_cpu_chip
      "${CMAKE_CURRENT_LIST_DIR}/../arch/${_kos_tc_arch}/chip/${KICKOS_CHIP}/cpu.cmake")
  if(EXISTS "${_kos_tc_cpu_chip}")
    include("${_kos_tc_cpu_chip}")
  endif()
  if(NOT DEFINED KICKOS_MCPU)
    message(FATAL_ERROR "KickOS ${_kos_tc_label} toolchain: board '${KICKOS_BOARD}' resolved "
      "no CPU baseline. Neither boards/${KICKOS_BOARD}/board.cmake nor "
      "arch/${_kos_tc_arch}/chip/${KICKOS_CHIP}/cpu.cmake states one (is it the sim? use the "
      "host toolchain for that)")
  endif()
  set(_kos_cpu ${KICKOS_MCPU})
endmacro()

# kickos_arm_cpu(FLOAT <soft|softfp|hard> MCPU <flags...>)
#
# What an arch/arm/chip/<chip>/cpu.cmake states. The FLOAT default is guarded: the board
# descriptor is read first and a board that states its own ABI wins.
macro(kickos_arm_cpu)
  cmake_parse_arguments(_kos_ac "" "FLOAT" "MCPU" ${ARGN})
  if(NOT _kos_ac_MCPU)
    message(FATAL_ERROR "kickos_arm_cpu: MCPU required")
  endif()
  if(NOT _kos_ac_FLOAT)
    message(FATAL_ERROR "kickos_arm_cpu: FLOAT required (soft, softfp or hard)")
  endif()
  set(KICKOS_MCPU ${_kos_ac_MCPU})
  if(NOT DEFINED KICKOS_MFLOAT_ABI)
    set(KICKOS_MFLOAT_ABI ${_kos_ac_FLOAT})
  endif()
endmacro()

# kickos_toolchain_export_baseline(<cpu-flags>)
macro(kickos_toolchain_export_baseline _kos_tc_cpu)
  set(KICKOS_ARCH "${KICKOS_ARCH}" CACHE STRING
      "KickOS arch backend selected by this toolchain")
  set(KICKOS_MCPU_FLAGS "${_kos_tc_cpu}" CACHE INTERNAL "Per-board CPU/ISA baseline")
endmacro()

# kickos_toolchain_cross_programs(<tool-prefix> <hint-cache-var>)
#
# The hint is seeded from the environment; left empty, HINTS contributes nothing and PATH
# decides, and a pinned install SHADOWS an on-PATH toolchain. It is then re-exported to the
# environment for CMake's compiler-ABI probe, which re-reads the toolchain file in a SEPARATE
# cmake process with a fresh cache: that inherits the environment and PATH, never a -D cache
# entry.
#
# These finds prove a program by that NAME exists, nothing more.
macro(kickos_toolchain_cross_programs _kos_tc_prefix _kos_tc_hint)
  set(${_kos_tc_hint} "$ENV{${_kos_tc_hint}}" CACHE PATH
      "Directory holding the ${_kos_tc_prefix}-* programs (empty => use PATH)")
  set(ENV{${_kos_tc_hint}} "${${_kos_tc_hint}}")

  find_program(CMAKE_C_COMPILER   ${_kos_tc_prefix}-gcc
               HINTS "${${_kos_tc_hint}}" REQUIRED)
  find_program(CMAKE_CXX_COMPILER ${_kos_tc_prefix}-g++
               HINTS "${${_kos_tc_hint}}" REQUIRED)
  find_program(CMAKE_ASM_COMPILER ${_kos_tc_prefix}-gcc
               HINTS "${${_kos_tc_hint}}" REQUIRED)
  find_program(CMAKE_OBJCOPY      ${_kos_tc_prefix}-objcopy
               HINTS "${${_kos_tc_hint}}" REQUIRED)
  find_program(CMAKE_SIZE         ${_kos_tc_prefix}-size
               HINTS "${${_kos_tc_hint}}")
endmacro()

# kickos_toolchain_flags_init(<flags>)
macro(kickos_toolchain_flags_init _kos_tc_flags)
  set(CMAKE_C_FLAGS_INIT   "${_kos_tc_flags}")
  set(CMAKE_CXX_FLAGS_INIT "${_kos_tc_flags}")
  set(CMAKE_ASM_FLAGS_INIT "${_kos_tc_flags}")
endmacro()

# kickos_toolchain_newlib_flags()
#
# After kickos_toolchain_flags_init, with kickos_require_dynreent_newlib's results in scope.
macro(kickos_toolchain_newlib_flags)
  string(APPEND CMAKE_C_FLAGS_INIT " ${_kos_newlib_c}")
  string(APPEND CMAKE_ASM_FLAGS_INIT " ${_kos_newlib_c}")
  string(APPEND CMAKE_CXX_FLAGS_INIT " ${_kos_newlib_cxx}")
  set(CMAKE_EXE_LINKER_FLAGS_INIT "${_kos_newlib_link}")
endmacro()

# kickos_toolchain_bare_metal_rules()
#
# STATIC_LIBRARY try-compile: the board's linker script and startup are supplied only at the
# application-link step.
#
# The Generic platform does not predefine the LINK_GROUP RESCAN feature that the
# arch/kernel/chip archive cycle needs; GNU ld provides it via --start-group/--end-group.
macro(kickos_toolchain_bare_metal_rules)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

  foreach(_kos_tc_lang C CXX ASM)
    set(CMAKE_${_kos_tc_lang}_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
    set(CMAKE_${_kos_tc_lang}_LINK_GROUP_USING_RESCAN
        "LINKER:--start-group" "LINKER:--end-group")
  endforeach()

  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
endmacro()
