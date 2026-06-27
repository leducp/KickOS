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

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# The board picks the arch; the *chip* picks the exact core + FPU. Both F103
# (Cortex-M3, no FPU) and F411 (Cortex-M4F) resolve to the armv7m arch but need
# different -mcpu, so the CPU flags key off the board, not the arch.
set(KICKOS_BOARD "frdmk64f" CACHE STRING "Target board: qemu|frdmk64f|f411disco|bluepill|picopi")

if(KICKOS_BOARD STREQUAL "qemu")
  # QEMU mps2-an386 (Cortex-M4F): the runnable armv7m verification target.
  set(KICKOS_ARCH "armv7m")
  set(_kos_cpu -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp)
elseif(KICKOS_BOARD STREQUAL "frdmk64f")
  set(KICKOS_ARCH "armv7m")
  set(_kos_cpu -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp)
elseif(KICKOS_BOARD STREQUAL "f411disco")
  set(KICKOS_ARCH "armv7m")
  set(_kos_cpu -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp)
elseif(KICKOS_BOARD STREQUAL "bluepill")
  set(KICKOS_ARCH "armv7m")
  set(_kos_cpu -mcpu=cortex-m3 -mfloat-abi=soft)
elseif(KICKOS_BOARD STREQUAL "picopi")
  set(KICKOS_ARCH "armv6m")
  set(_kos_cpu -mcpu=cortex-m0plus -mfloat-abi=soft)
else()
  message(FATAL_ERROR "KickOS arm toolchain: unknown board '${KICKOS_BOARD}'")
endif()

set(KICKOS_ARCH   "${KICKOS_ARCH}" CACHE STRING "KickOS arch backend selected by this toolchain")
set(KICKOS_IS_SIM OFF              CACHE BOOL   "Target is the host sim")

find_program(CMAKE_C_COMPILER   arm-none-eabi-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER arm-none-eabi-g++ REQUIRED)
find_program(CMAKE_ASM_COMPILER arm-none-eabi-gcc REQUIRED)
find_program(CMAKE_OBJCOPY      arm-none-eabi-objcopy REQUIRED)
find_program(CMAKE_SIZE         arm-none-eabi-size)

# The compiler cannot produce a runnable executable without the board's linker
# script + startup, which are not present during CMake's compiler probe. Probe
# with a static library instead so configure succeeds standalone (a step
# boundary must always configure).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -mthumb: Cortex-M is Thumb-only. -ffunction/-fdata-sections + --gc-sections
# (at link) drop unreferenced code so the image is minimal.
string(JOIN " " _kos_common ${_kos_cpu} -mthumb -ffunction-sections -fdata-sections)
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
