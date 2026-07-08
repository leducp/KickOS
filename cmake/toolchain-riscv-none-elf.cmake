# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS RISC-V targets (riscv64-unknown-elf, the distro
# GNU RISC-V cross compiler -- multilib, so `-march=rv32imac -mabi=ilp32` selects
# the rv32imac/ilp32 newlib/libgcc). Sibling of toolchain-rx-elf.cmake: bare-metal
# freestanding, no host libc, no default startfiles. The board/chip layer supplies
# the linker script + startup at the application-link step; here we pin the cross
# compiler and the RV32IMAC ISA/ABI baseline so the right multilib is selected
# uniformly for compile AND link.
#
# This file introduces the family value "riscv" (arm|rx|xtensa|riscv). The
# per-board arch/chip/CPU facts live in a board descriptor
# (boards/<board>/board.cmake) that this file includes -- the same seam every
# other family's toolchain uses. RV32IMAC is a clean standard ISA (no windowed
# ABI, no vendor fork): a stock riscv64-unknown-elf gcc is the clean-room choice,
# mirroring how the ARM family uses apt's gcc-arm-none-eabi.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv)

set(KICKOS_BOARD "qemu-riscv" CACHE STRING "Target board: qemu-riscv | esp32c6-wroom")

# Board descriptor: sets KICKOS_ARCH / KICKOS_ARCH_FAMILY / KICKOS_CHIP and the
# per-board CPU flags (KICKOS_MCPU). In-tree it lives under boards/<board>/; an
# installed single-board package ships the one board it was built for beside this
# toolchain file (root CMakeLists install).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake") # in-tree
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  set(KICKOS_BOARD "${KICKOS_BOARD_ID}" CACHE STRING "Target board" FORCE)
else()
  message(FATAL_ERROR "KickOS riscv toolchain: no board descriptor for '${KICKOS_BOARD}'")
endif()

if(NOT DEFINED KICKOS_MCPU)
  message(FATAL_ERROR "KickOS riscv toolchain: board '${KICKOS_BOARD}' has no KICKOS_MCPU")
endif()
set(_kos_cpu ${KICKOS_MCPU})

set(KICKOS_ARCH        "${KICKOS_ARCH}"        CACHE STRING "KickOS arch backend selected by this toolchain")
set(KICKOS_ARCH_FAMILY "riscv"                 CACHE STRING "KickOS ISA family (arm|rx|xtensa|riscv)")

# Exported so sub-links that bypass the normal compile path reuse the exact same
# ISA baseline instead of hardcoding a value that could drift from here.
set(KICKOS_MCPU_FLAGS "${_kos_cpu}" CACHE INTERNAL "Per-board RISC-V ISA baseline")

# The distro cross compiler is on PATH (/usr/bin, apt gcc-riscv64-unknown-elf);
# overridable + PATH-searched for a host that installs it elsewhere.
set(KICKOS_RISCV_TOOLCHAIN_BIN "" CACHE PATH "Directory holding the riscv64-unknown-elf-* programs")

find_program(CMAKE_C_COMPILER   riscv64-unknown-elf-gcc     HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_CXX_COMPILER riscv64-unknown-elf-g++     HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_ASM_COMPILER riscv64-unknown-elf-gcc     HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_OBJCOPY      riscv64-unknown-elf-objcopy HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_SIZE         riscv64-unknown-elf-size    HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}")

# No linker script + startup during CMake's compiler probe (the board supplies
# them at the app-link step), so probe with a static library -- a step boundary
# must always configure standalone.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -ffunction/-fdata-sections + --gc-sections (at link) drop unreferenced code.
# ${_kos_cpu} = -march=rv32imac -mabi=ilp32 (the board descriptor): the same flags
# on compile AND link pick the matching rv32imac/ilp32 multilib (libgcc/newlib) so
# the soft-float + 64-bit-divide helpers resolve.
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

# Bare-metal search rules: toolchain sysroot, never the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
