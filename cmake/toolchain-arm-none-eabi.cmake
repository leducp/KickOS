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
# The compiler this file wants is the official Arm GNU Toolchain (arm-none-eabi,
# newlib-based, ships libstdc++/libsupc++): the full-C++ opt-in needs newlib's full
# libstdc++, which Debian's picolibc-based apt toolchain cannot provide. The
# capability check after the finds is what ENFORCES that. Whatever gets resolved
# (hint, PATH, or -D) is refused at configure time unless it has newlib + libstdc++
# for THIS board's multilib. The hint is convenience and reproducibility only.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# The board picks the arch; the *chip* picks the exact core + FPU, and says so in its own
# arch/arm/chip/<chip>/cpu.cmake. Both F103 (Cortex-M3, no FPU) and F411 (Cortex-M4F)
# resolve to the armv7m arch and need different -mcpu, so the flags key off the chip and
# not the arch. The board descriptor states arch + chip, and a CPU flag only where the
# board itself differs from its chip; both are read here pre-project() and by the build's
# board resolver (cmake/kickos.cmake), so the toolchain and the build cannot disagree.
set(KICKOS_BOARD "frdmk64f" CACHE STRING "Target board: qemu|frdmk64f|f411disco|bluepill-c8|picopi")

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


# The chip's own CPU baseline, for whatever the board left unset. It is a chip fact:
# `board` states the arch, the CHIP states the core and its FPU. Sibling of the caps.cmake
# and mpu.cmake this tree already keeps per chip, and included AFTER the descriptor so a
# board that genuinely differs (a float ABI, or the mps2 boards' emulated core) wins.
# An installed package has no arch/ tree and ships a descriptor with the flags already
# resolved into it, so a missing file here is not an error; a missing VALUE is, below.
set(_kos_cpu_chip "${CMAKE_CURRENT_LIST_DIR}/../arch/arm/chip/${KICKOS_CHIP}/cpu.cmake")
if(EXISTS "${_kos_cpu_chip}")
  include("${_kos_cpu_chip}")
endif()

# A bare-metal ARM board must end up with both. The sim descriptor (KICKOS_ARCH=sim) has
# no chip and no cpu.cmake, so a misdirected -DKICKOS_BOARD=sim (or any non-MCU board)
# under the ARM toolchain is caught up front, not as an opaque later failure.
if(NOT DEFINED KICKOS_MCPU OR NOT DEFINED KICKOS_MFLOAT_ABI)
  message(FATAL_ERROR "KickOS arm toolchain: board '${KICKOS_BOARD}' resolved no CPU "
    "baseline. Neither boards/${KICKOS_BOARD}/board.cmake nor "
    "arch/arm/chip/${KICKOS_CHIP}/cpu.cmake states -mcpu and the float ABI (is it the "
    "sim? use the host toolchain for that)")
endif()
set(_kos_cpu ${KICKOS_MCPU} -mfloat-abi=${KICKOS_MFLOAT_ABI})

set(KICKOS_ARCH   "${KICKOS_ARCH}" CACHE STRING "KickOS arch backend selected by this toolchain")

# The per-chip CPU baseline, exported so sub-links that bypass the normal compile
# path (e.g. the RP2040 boot2 second-stage link in arch/CMakeLists.txt) reuse the
# exact same -mcpu/-mfpu instead of hardcoding a value that could drift from here.
set(KICKOS_MCPU_FLAGS "${_kos_cpu}" CACHE INTERNAL "Per-chip -mcpu/-mfpu baseline")

# The prebuilt Arm GNU Toolchain location, seeded from the environment so no
# contributor's home directory is baked into the repo (export KICKOS_ARM_TOOLCHAIN_BIN
# once, or pass -D). Left empty, HINTS contributes nothing and PATH decides. A pinned
# install SHADOWS an on-PATH toolchain.
set(KICKOS_ARM_TOOLCHAIN_BIN
    "$ENV{KICKOS_ARM_TOOLCHAIN_BIN}"
    CACHE PATH "Directory holding the arm-none-eabi-* programs (empty => use PATH)")

# Re-export the resolved hint: CMake's compiler-ABI probe re-reads this file in a
# SEPARATE cmake process with a fresh cache, which inherits the environment and PATH
# but never a -D cache entry. Re-exporting makes -D, the environment and a
# reconfigure agree. An empty value clears the variable, leaving PATH to decide.
set(ENV{KICKOS_ARM_TOOLCHAIN_BIN} "${KICKOS_ARM_TOOLCHAIN_BIN}")

find_program(CMAKE_C_COMPILER   arm-none-eabi-gcc     HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_CXX_COMPILER arm-none-eabi-g++     HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_ASM_COMPILER arm-none-eabi-gcc     HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_OBJCOPY      arm-none-eabi-objcopy HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_SIZE         arm-none-eabi-size    HINTS "${KICKOS_ARM_TOOLCHAIN_BIN}")

# The finds prove a program NAMED arm-none-eabi-g++ exists, not that it can build
# KickOS (Debian's on-PATH arm-none-eabi is a C-only picolibc build). ${_kos_cpu} +
# -mthumb make the probe resolve THIS board's multilib, not the compiler's default.
include("${CMAKE_CURRENT_LIST_DIR}/toolchain-cxx-runtime-check.cmake")
kickos_require_usable_cross_cxx("arm" "${CMAKE_CXX_COMPILER}"
  KICKOS_ARM_TOOLCHAIN_BIN
  "https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi.tar.xz"
  ${_kos_cpu} -mthumb)

# The compiler cannot produce a runnable executable without the board's linker
# script + startup, which are not present during CMake's compiler probe. Probe
# with a static library instead so configure succeeds standalone (a step
# boundary must always configure).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -mthumb: Cortex-M is Thumb-only. -ffunction/-fdata-sections + --gc-sections
# (at link) drop unreferenced code so the image is minimal. -mno-unaligned-access:
# some parts (K64F) forbid unaligned/burst accesses across a RAM bank boundary
# (0x2000_0000, SRAM_L|SRAM_U), so the compiler must never emit one.
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
