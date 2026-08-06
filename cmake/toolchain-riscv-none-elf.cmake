# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS RISC-V targets (riscv32-none-elf, the pinned
# RISCStar GNU RISC-V cross compiler: newlib, multilib, so `-march=rv32imac
# -mabi=ilp32` selects the rv32imac/ilp32 SOFT-FLOAT newlib/libgcc + libstdc++
# out of the box). Sibling of toolchain-rx-elf.cmake: bare-metal freestanding, no
# host libc, no default startfiles. The board/chip layer supplies the linker
# script + startup at the application-link step; here we pin the cross compiler
# and the RV32IMAC ISA/ABI baseline so the right multilib is selected uniformly
# for compile AND link.
#
# This file introduces the family value "riscv" (arm|rx|xtensa|riscv). The
# per-board arch/chip/CPU facts live in a board descriptor
# (boards/<board>/board.cmake) that this file includes, the same seam every
# other family's toolchain uses. RISCStar is the reproducible prebuilt this file
# targets (newlib, rv32imac/ilp32 soft-float multilib with libstdc++ OOTB). The
# soft-float multilib is the C6-safety guarantee: no F/D instructions leak into the
# image. The capability check after the finds is what ENFORCES newlib + libstdc++
# for THIS board's multilib, whatever gets resolved (hint, PATH, or -D); the hint
# is convenience and reproducibility only.

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


# The chip's own CPU baseline, included AFTER the board descriptor so a board that states
# its own float ABI wins. A chip whose core really is per-board ships no cpu.cmake at all
# (mps2).
# An installed package has no arch/ tree and ships a descriptor with the flags already
# resolved into it, so a missing file here is not an error; a missing VALUE is, below.
set(_kos_cpu_chip "${CMAKE_CURRENT_LIST_DIR}/../arch/riscv/chip/${KICKOS_CHIP}/cpu.cmake")
if(EXISTS "${_kos_cpu_chip}")
  include("${_kos_cpu_chip}")
endif()

if(NOT DEFINED KICKOS_MCPU)
  message(FATAL_ERROR "KickOS riscv toolchain: board '${KICKOS_BOARD}' resolved no ISA "
    "baseline from boards/${KICKOS_BOARD}/board.cmake or "
    "arch/riscv/chip/${KICKOS_CHIP}/cpu.cmake")
endif()
set(_kos_cpu ${KICKOS_MCPU})

set(KICKOS_ARCH        "${KICKOS_ARCH}"        CACHE STRING "KickOS arch backend selected by this toolchain")
set(KICKOS_ARCH_FAMILY "riscv"                 CACHE STRING "KickOS ISA family (arm|rx|xtensa|riscv)")

# Exported so sub-links that bypass the normal compile path reuse the exact same
# ISA baseline instead of hardcoding a value that could drift from here.
set(KICKOS_MCPU_FLAGS "${_kos_cpu}" CACHE INTERNAL "Per-board RISC-V ISA baseline")

# The RISCStar cross compiler location, seeded from the environment so no
# contributor's home directory is baked into the repo (export
# KICKOS_RISCV_TOOLCHAIN_BIN once, or pass -D). Left empty, HINTS contributes
# nothing and PATH decides. A pinned install SHADOWS an on-PATH toolchain.
set(KICKOS_RISCV_TOOLCHAIN_BIN
    "$ENV{KICKOS_RISCV_TOOLCHAIN_BIN}"
    CACHE PATH "Directory holding the riscv32-none-elf-* programs (empty => use PATH)")

# Re-export the resolved hint: CMake's compiler-ABI probe re-reads this file in a
# SEPARATE cmake process with a fresh cache, which inherits the environment and PATH
# but never a -D cache entry. Re-exporting makes -D, the environment and a
# reconfigure agree. An empty value clears the variable, leaving PATH to decide.
set(ENV{KICKOS_RISCV_TOOLCHAIN_BIN} "${KICKOS_RISCV_TOOLCHAIN_BIN}")

find_program(CMAKE_C_COMPILER   riscv32-none-elf-gcc     HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_CXX_COMPILER riscv32-none-elf-g++     HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_ASM_COMPILER riscv32-none-elf-gcc     HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_OBJCOPY      riscv32-none-elf-objcopy HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_SIZE         riscv32-none-elf-size    HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}")

# The finds prove a program NAMED riscv32-none-elf-g++ exists, not that it can
# build KickOS (distro rv32 cross builds are routinely C-only picolibc).
# ${_kos_cpu} makes the probe resolve THIS board's multilib (soft-float
# rv32imac/ilp32), not the compiler's default.
include("${CMAKE_CURRENT_LIST_DIR}/toolchain-cxx-runtime-check.cmake")
kickos_require_usable_cross_cxx("riscv" "${CMAKE_CXX_COMPILER}"
  KICKOS_RISCV_TOOLCHAIN_BIN
  "https://releases.riscstar.com/toolchain/16.1-r1/riscstar-toolchain-16.1-r1-x86_64-riscv32-none-elf.tar.xz"
  ${_kos_cpu})

# No linker script + startup during CMake's compiler probe (the board supplies
# them at the app-link step), so probe with a static library. A step boundary
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
