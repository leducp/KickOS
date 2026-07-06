# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS Renesas RX targets (rx-elf, GNU RX).
#
# Sibling of toolchain-arm-none-eabi.cmake: bare-metal freestanding, no host
# libc, no default startfiles. The final link recipe (startup object + linker
# script) is supplied by the board/chip layer at the application-link step; here
# we pin the cross compiler and the RXv3 ISA/endianness baseline so the right
# multilib is selected uniformly for compile AND link.
#
# The ARM toolchain hardcodes an ISA *family* of "arm"; this file introduces the
# third family value, "rx". The per-board arch/chip/CPU facts live in a board
# descriptor (boards/<board>/board.cmake) that this file includes -- the same
# board-descriptor seam the ARM toolchain uses.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR rx)

set(KICKOS_BOARD "rx72m" CACHE STRING "Target board: rx72m")

# Board descriptor: sets KICKOS_ARCH / KICKOS_ARCH_FAMILY / KICKOS_CHIP and the
# per-board CPU flags (KICKOS_MCPU). Externalized (like the arm toolchain) so a
# second RX board is one new descriptor, no edit here. In-tree it lives under
# boards/<board>/; an installed single-board package ships the one board it was
# built for beside this toolchain file (root CMakeLists install).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake") # in-tree
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  # Installed single-board package: the one shipped descriptor is authoritative.
  # Adopt its board id; a genuine request for a DIFFERENT board must fail (the
  # package ships exactly one arch/chip/linker), not silently mislabel.
  include("${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  if(NOT KICKOS_BOARD STREQUAL "rx72m"
     AND NOT KICKOS_BOARD STREQUAL "${KICKOS_BOARD_ID}")
    message(FATAL_ERROR "KickOS: this package provides board '${KICKOS_BOARD_ID}', "
      "not '${KICKOS_BOARD}' -- a KickOS MCU package is single-board")
  endif()
  set(KICKOS_BOARD "${KICKOS_BOARD_ID}" CACHE STRING "Target board" FORCE)
else()
  message(FATAL_ERROR "KickOS rx toolchain: no board descriptor for '${KICKOS_BOARD}'")
endif()

if(NOT DEFINED KICKOS_MCPU)
  message(FATAL_ERROR "KickOS rx toolchain: board '${KICKOS_BOARD}' has no KICKOS_MCPU")
endif()
set(_kos_cpu ${KICKOS_MCPU})

set(KICKOS_ARCH        "${KICKOS_ARCH}"        CACHE STRING "KickOS arch backend selected by this toolchain")
set(KICKOS_ARCH_FAMILY "${KICKOS_ARCH_FAMILY}" CACHE STRING "KickOS ISA family (arm|rx)")

# Exported so sub-links that bypass the normal compile path reuse the exact same
# ISA baseline instead of hardcoding a value that could drift from here.
set(KICKOS_MCPU_FLAGS "${_kos_cpu}" CACHE INTERNAL "Per-board RX ISA baseline")

# The prebuilt GNU RX toolchain location. Overridable; also honours PATH.
set(KICKOS_RX_TOOLCHAIN_BIN
    "/home/leduc/Apps/toolchains/gcc_14.2.0.202511_rx_elf/bin"
    CACHE PATH "Directory holding the rx-elf-* programs")

find_program(CMAKE_C_COMPILER   rx-elf-gcc     HINTS "${KICKOS_RX_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_CXX_COMPILER rx-elf-g++     HINTS "${KICKOS_RX_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_ASM_COMPILER rx-elf-gcc     HINTS "${KICKOS_RX_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_OBJCOPY      rx-elf-objcopy HINTS "${KICKOS_RX_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_SIZE         rx-elf-size    HINTS "${KICKOS_RX_TOOLCHAIN_BIN}")

# The compiler cannot produce a runnable executable without the board's linker
# script + startup, which are absent during CMake's compiler probe. Probe with a
# static library so configure succeeds standalone (a step boundary must configure).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -ffunction/-fdata-sections + --gc-sections (at link) drop unreferenced code so
# the image is minimal. RX instructions are always little-endian; only *data*
# endianness is selectable and GNU RX defaults to little-endian, matching the
# MDE option word the linker script emits (spike §1, §6).
string(JOIN " " _kos_common ${_kos_cpu} -ffunction-sections -fdata-sections)
set(CMAKE_C_FLAGS_INIT   "${_kos_common}")
set(CMAKE_CXX_FLAGS_INIT "${_kos_common}")
set(CMAKE_ASM_FLAGS_INIT "${_kos_common}")

# The Generic (bare-metal) platform doesn't predefine the LINK_GROUP RESCAN
# feature the arch<->kernel<->chip archive cycle needs; GNU ld provides it via
# --start-group/--end-group. Declare it for every possible link language.
foreach(_lang C CXX ASM)
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
endforeach()

# Bare-metal search rules: find headers/libs in the toolchain sysroot, never host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
