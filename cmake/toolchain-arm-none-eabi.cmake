# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS ARM Cortex-M targets (arm-none-eabi).
#
# Unlike the sim (which links as a hosted ELF against host libc), the MCU build
# is bare-metal freestanding: no host libc, no default startfiles, no default
# link. The final link recipe (startup object + linker script) is supplied by
# the board/chip layer at the application-link step; here we only pin the
# cross compiler and the per-chip -mcpu/-mfpu/-mfloat-abi baseline so the right
# multilib is selected uniformly for compile AND link.
#
# This file prefers the pinned Apps Arm GNU Toolchain (the official Arm
# arm-none-eabi build, newlib-based: --with-newlib, ships libstdc++/libsupc++,
# nano.specs, rmprofile multilibs) over whatever arm-none-eabi-gcc happens to be
# on PATH. Reasons: reproducibility (one pinned compiler across the fleet) and
# newlib's full libstdc++ -- required for the eventual full-C++ opt-in, which
# Debian's picolibc-based apt toolchain cannot provide. It still falls back to an
# on-PATH arm-none-eabi-gcc (the finds keep PATH search) so a machine without the
# Apps directory -- e.g. CI -- still resolves a toolchain.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# The board picks the arch; the *chip* picks the exact core + FPU. Both F103
# (Cortex-M3, no FPU) and F411 (Cortex-M4F) resolve to the armv7m arch but need
# different -mcpu, so the CPU flags key off the board, not the arch. That fact
# (arch + chip + -mcpu) lives in ONE place per board: boards/<board>/board.cmake,
# included here (pre-project, for -mcpu) and by the build's board resolver
# (cmake/kickos.cmake) so the toolchain and the build can never disagree.
set(KICKOS_BOARD "frdmk64f" CACHE STRING "Target board: qemu|frdmk64f|f411disco|bluepill|picopi")

# In-tree the descriptor is boards/<board>/board.cmake relative to the repo root
# (this file lives in <repo>/cmake). An installed MCU package ships the one board
# it was built for beside this toolchain file (see the root CMakeLists install).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake") # in-tree
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  # Installed single-board package: the one shipped descriptor is authoritative.
  # Adopt its board id so the -mcpu + KICKOS_BOARD label are the package's, not
  # this file's frdmk64f default; a genuine request for a DIFFERENT board must
  # fail (the package ships exactly one arch/chip/linker), not silently mislabel.
  include("${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  if(NOT KICKOS_BOARD STREQUAL "frdmk64f"
     AND NOT KICKOS_BOARD STREQUAL "${KICKOS_BOARD_ID}")
    message(FATAL_ERROR "KickOS: this package provides board '${KICKOS_BOARD_ID}', "
      "not '${KICKOS_BOARD}' -- a KickOS MCU package is single-board")
  endif()
  set(KICKOS_BOARD "${KICKOS_BOARD_ID}" CACHE STRING "Target board" FORCE)
else()
  message(FATAL_ERROR "KickOS arm toolchain: no board descriptor for '${KICKOS_BOARD}'")
endif()

# A bare-metal ARM board's descriptor must define KICKOS_MCPU. The sim descriptor
# (KICKOS_ARCH=sim) does not -- catch a misdirected -DKICKOS_BOARD=sim (or any
# non-MCU board) under the ARM toolchain up front, not as an opaque later failure.
if(NOT DEFINED KICKOS_MCPU)
  message(FATAL_ERROR "KickOS arm toolchain: board '${KICKOS_BOARD}' has no "
    "KICKOS_MCPU (is it the sim? use the host toolchain for that)")
endif()
set(_kos_cpu ${KICKOS_MCPU})

set(KICKOS_ARCH   "${KICKOS_ARCH}" CACHE STRING "KickOS arch backend selected by this toolchain")

# The per-chip CPU baseline, exported so sub-links that bypass the normal compile
# path (e.g. the RP2040 boot2 second-stage link in arch/CMakeLists.txt) reuse the
# exact same -mcpu/-mfpu instead of hardcoding a value that could drift from here.
set(KICKOS_MCPU_FLAGS "${_kos_cpu}" CACHE INTERNAL "Per-chip -mcpu/-mfpu baseline")

# The pinned prebuilt Arm GNU (newlib) toolchain location. Overridable; the finds
# also honour PATH, so a host without this directory falls back to an on-PATH
# arm-none-eabi-gcc (e.g. CI).
set(KICKOS_ARM_TOOLCHAIN_BIN
    "/home/leduc/Apps/toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi/bin"
    CACHE PATH "Directory holding the arm-none-eabi-* programs")

find_program(CMAKE_C_COMPILER   arm-none-eabi-gcc     HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_CXX_COMPILER arm-none-eabi-g++     HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_ASM_COMPILER arm-none-eabi-gcc     HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_OBJCOPY      arm-none-eabi-objcopy HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_SIZE         arm-none-eabi-size    HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}")

# The compiler cannot produce a runnable executable without the board's linker
# script + startup, which are not present during CMake's compiler probe. Probe
# with a static library instead so configure succeeds standalone (a step
# boundary must always configure).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -mthumb: Cortex-M is Thumb-only. -ffunction/-fdata-sections + --gc-sections
# (at link) drop unreferenced code so the image is minimal. -mno-unaligned-access:
# some parts (K64F) forbid unaligned/burst accesses across a RAM bank boundary
# (0x2000_0000, SRAM_L|SRAM_U) -- force the compiler to never emit one.
string(JOIN " " _kos_common ${_kos_cpu} -mthumb -mno-unaligned-access
       -ffunction-sections -fdata-sections)
set(CMAKE_C_FLAGS_INIT   "${_kos_common}")
set(CMAKE_CXX_FLAGS_INIT "${_kos_common}")
set(CMAKE_ASM_FLAGS_INIT "${_kos_common}")

# The Generic (bare-metal) platform doesn't predefine the LINK_GROUP RESCAN
# feature that the arch<->kernel<->chip archive cycle needs; GNU ld provides it
# via --start-group/--end-group. Declare it for every possible link language.
foreach(_lang C CXX ASM)
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
endforeach()

# Bare-metal search rules: find headers/libs in the toolchain sysroot, never on
# the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
