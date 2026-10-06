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
  # Only the Arm family carries a nano profile beside the full one.
  if(DEFINED KICKOS_BOARD_NEWLIB
     AND NOT (KICKOS_BOARD_NEWLIB STREQUAL "nano" AND "${_kos_tc_label}" STREQUAL "arm"))
    message(FATAL_ERROR "KickOS ${_kos_tc_label} toolchain: board '${KICKOS_BOARD}' names "
      "newlib profile '${KICKOS_BOARD_NEWLIB}'; a descriptor names `nano`, on an Arm board, or "
      "nothing")
  endif()
  if(DEFINED KICKOS_FULL_NEWLIB AND NOT KICKOS_BOARD_NEWLIB STREQUAL "nano")
    message(FATAL_ERROR "KickOS ${_kos_tc_label} toolchain: KICKOS_FULL_NEWLIB chooses between "
      "the profiles of a board whose descriptor names nano, and board '${KICKOS_BOARD}' links "
      "the full newlib only")
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

# kickos_toolchain_package(<triple>)
#
# The family's compiler and binutils, from the KickOS toolchain folder KICKOS_TOOLCHAIN names
# (docs/design-m10-toolchain.md), which tools/kickos-toolchain.sh provisions: its index gives
# each family's bin directory, and every program is found there alone, as _kos_tc_bin in the
# caller. The environment seeds the cache entry, and the entry is re-exported to the
# environment for CMake's compiler-ABI probe, which re-reads the toolchain file in a SEPARATE
# cmake process with a fresh cache: that inherits the environment, never a -D cache entry.
macro(kickos_toolchain_package _kos_tc_triple)
  set(KICKOS_TOOLCHAIN "$ENV{KICKOS_TOOLCHAIN}" CACHE PATH
      "The KickOS toolchain folder, holding kickos-toolchain.cmake")
  set(ENV{KICKOS_TOOLCHAIN} "${KICKOS_TOOLCHAIN}")
  set(_kos_tc_how "Provision it from the KickOS source root with\n"
      "  tools/kickos-toolchain.sh <dir> [family,family,...]\n"
      "  . <dir>/kickos-toolchain.sh\n"
      "naming ${_kos_tc_triple} among the families, and configure again in that shell.")
  if(KICKOS_TOOLCHAIN STREQUAL "")
    message(FATAL_ERROR "KickOS: KICKOS_TOOLCHAIN is unset, and it names the KickOS "
      "toolchain every board builds with. " ${_kos_tc_how})
  endif()
  if(NOT EXISTS "${KICKOS_TOOLCHAIN}/kickos-toolchain.cmake")
    message(FATAL_ERROR "KickOS: KICKOS_TOOLCHAIN=${KICKOS_TOOLCHAIN} holds no "
      "kickos-toolchain.cmake, so it is not a KickOS toolchain folder. " ${_kos_tc_how})
  endif()
  include("${KICKOS_TOOLCHAIN}/kickos-toolchain.cmake")
  if(NOT DEFINED KICKOS_TOOLCHAIN_BIN_${_kos_tc_triple})
    message(FATAL_ERROR "KickOS: the toolchain in ${KICKOS_TOOLCHAIN} carries no "
      "${_kos_tc_triple} compiler. " ${_kos_tc_how})
  endif()
  file(REAL_PATH "${KICKOS_TOOLCHAIN_BIN_${_kos_tc_triple}}" _kos_tc_bin)
  # find_program leaves a cached compiler alone, so a build directory that resolved another
  # one would keep it. CMake cannot change a tree's compiler.
  foreach(_kos_tc_prog IN ITEMS CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_ASM_COMPILER)
    if(NOT "$CACHE{${_kos_tc_prog}}" STREQUAL "")
      get_filename_component(_kos_tc_dir "$CACHE{${_kos_tc_prog}}" DIRECTORY)
      file(REAL_PATH "${_kos_tc_dir}" _kos_tc_dir)
      if(NOT _kos_tc_dir STREQUAL _kos_tc_bin)
        message(FATAL_ERROR "KickOS: this build directory's ${_kos_tc_prog} is "
          "$CACHE{${_kos_tc_prog}}, not the ${_kos_tc_triple} compiler of the toolchain in "
          "${KICKOS_TOOLCHAIN}. Configure a fresh build directory.")
      endif()
    endif()
  endforeach()
  find_program(CMAKE_C_COMPILER   ${_kos_tc_triple}-gcc     HINTS "${_kos_tc_bin}"
               NO_DEFAULT_PATH REQUIRED)
  find_program(CMAKE_CXX_COMPILER ${_kos_tc_triple}-g++     HINTS "${_kos_tc_bin}"
               NO_DEFAULT_PATH REQUIRED)
  find_program(CMAKE_ASM_COMPILER ${_kos_tc_triple}-gcc     HINTS "${_kos_tc_bin}"
               NO_DEFAULT_PATH REQUIRED)
  find_program(CMAKE_OBJCOPY      ${_kos_tc_triple}-objcopy HINTS "${_kos_tc_bin}"
               NO_DEFAULT_PATH REQUIRED)
  find_program(CMAKE_SIZE         ${_kos_tc_triple}-size    HINTS "${_kos_tc_bin}"
               NO_DEFAULT_PATH)
endmacro()

# kickos_toolchain_flags_init(<flags>)
macro(kickos_toolchain_flags_init _kos_tc_flags)
  set(CMAKE_C_FLAGS_INIT   "${_kos_tc_flags}")
  set(CMAKE_CXX_FLAGS_INIT "${_kos_tc_flags}")
  set(CMAKE_ASM_FLAGS_INIT "${_kos_tc_flags}")
endmacro()

# kickos_toolchain_newlib_flags()
#
# After kickos_toolchain_flags_init, with kickos_require_toolchain_newlib's results in scope.
macro(kickos_toolchain_newlib_flags)
  # *_FLAGS_INIT only seats an entry the cache does not hold yet. An older build
  # directory, or a -D of one of these on the first configure, keeps stock libc
  # headers and the stock -lc search path; the image then runs against the wrong
  # libc. Refuse that state.
  foreach(_kos_cached IN ITEMS CMAKE_C_FLAGS CMAKE_ASM_FLAGS CMAKE_CXX_FLAGS
                               CMAKE_EXE_LINKER_FLAGS)
    if(DEFINED CACHE{${_kos_cached}})
      if(_kos_cached STREQUAL "CMAKE_C_FLAGS" OR _kos_cached STREQUAL "CMAKE_ASM_FLAGS")
        set(_kos_required "${_kos_newlib_c}")
      elseif(_kos_cached STREQUAL "CMAKE_CXX_FLAGS")
        set(_kos_required "${_kos_newlib_cxx}")
      else()
        set(_kos_required "${_kos_newlib_link}")
      endif()
      string(FIND "$CACHE{${_kos_cached}}" "${_kos_required}" _kos_at)
      if(_kos_at EQUAL -1)
        message(FATAL_ERROR "KickOS: ${_kos_cached} lacks the selected pinned newlib "
          "flags: the build directory predates them, or a -D${_kos_cached} replaced "
          "them. Configure a fresh build directory without that -D.")
      endif()
      # The other profile's flag, where the selected one requires none: full newlib's flags
      # are empty, so a tree first configured nano would pass the check above.
      if(_kos_newlib_refuse)
        string(FIND "$CACHE{${_kos_cached}}" "${_kos_newlib_refuse}" _kos_at)
        if(NOT _kos_at EQUAL -1)
          message(FATAL_ERROR "KickOS: ${_kos_cached} carries ${_kos_newlib_refuse}, the "
            "other newlib profile's flag, from an earlier configure of this build directory. "
            "Configure a fresh build directory to change the profile.")
        endif()
      endif()
    endif()
  endforeach()
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
