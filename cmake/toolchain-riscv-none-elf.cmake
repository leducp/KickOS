# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS RISC-V targets: the pinned RISCStar riscv32-none-elf cross
# compiler (newlib, multilib, soft float). Bare metal, no host libc, no default startfiles;
# the board/chip layer supplies the linker script and startup at the application-link step.
#
# The riscv32-none-elf triple also ships the RV64 multilib, so the rv64 boards resolve out of
# this same toolchain: it is the MULTILIB that names the XLEN here, never the triple.
#
# This file introduces the family value "riscv" (arm|rx|xtensa|riscv). The capability check
# after the finds is what ENFORCES newlib + libstdc++ for THIS board's multilib, whatever
# gets resolved; the hint is convenience only.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv)

set(KICKOS_BOARD "qemu-riscv" CACHE STRING "Target board: qemu-riscv | qemu-riscv64 | esp32c6-wroom")

# Board descriptor: sets KICKOS_ARCH / KICKOS_ARCH_FAMILY / KICKOS_CHIP and the per-board
# CPU flags. An installed single-board package ships it beside this toolchain file.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake") # in-tree
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  set(KICKOS_BOARD "${KICKOS_BOARD_ID}" CACHE STRING "Target board" FORCE)
else()
  message(FATAL_ERROR "KickOS riscv toolchain: no board descriptor for '${KICKOS_BOARD}'")
endif()


# The chip's own CPU baseline, included AFTER the board descriptor so a board that states
# its own float ABI wins. An installed package has no arch/ tree and ships a descriptor with
# the flags resolved into it, so a missing file here is not an error; a missing VALUE is.
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

# Exported so sub-links that bypass the normal compile path reuse the same ISA
# baseline.
set(KICKOS_MCPU_FLAGS "${_kos_cpu}" CACHE INTERNAL "Per-board RISC-V ISA baseline")

# Seeded from the environment so no contributor's home directory is baked into the repo.
# Left empty, HINTS contributes nothing and PATH decides; a pinned install SHADOWS PATH.
set(KICKOS_RISCV_TOOLCHAIN_BIN
    "$ENV{KICKOS_RISCV_TOOLCHAIN_BIN}"
    CACHE PATH "Directory holding the riscv32-none-elf-* programs (empty => use PATH)")

# Re-export the resolved hint: CMake's compiler-ABI probe re-reads this file in a SEPARATE
# cmake process with a fresh cache, which inherits the environment and PATH but never a -D
# cache entry. An empty value clears the variable, leaving PATH to decide.
set(ENV{KICKOS_RISCV_TOOLCHAIN_BIN} "${KICKOS_RISCV_TOOLCHAIN_BIN}")

find_program(CMAKE_C_COMPILER   riscv32-none-elf-gcc     HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_CXX_COMPILER riscv32-none-elf-g++     HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_ASM_COMPILER riscv32-none-elf-gcc     HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_OBJCOPY      riscv32-none-elf-objcopy HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}" REQUIRED)
find_program(CMAKE_SIZE         riscv32-none-elf-size    HINTS "${KICKOS_RISCV_TOOLCHAIN_BIN}")

# The finds prove a program NAMED riscv32-none-elf-g++ exists, not that it can build KickOS
# (distro rv32 cross builds are routinely C-only picolibc). ${_kos_cpu} makes the probe
# resolve THIS board's multilib.
include("${CMAKE_CURRENT_LIST_DIR}/toolchain-cxx-runtime-check.cmake")
kickos_require_usable_cross_cxx("riscv" "${CMAKE_CXX_COMPILER}"
  KICKOS_RISCV_TOOLCHAIN_BIN
  "https://releases.riscstar.com/toolchain/16.1-r1/riscstar-toolchain-16.1-r1-x86_64-riscv32-none-elf.tar.xz"
  ${_kos_cpu})

# No linker script and no startup during CMake's compiler probe, so probe with a static
# library.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The same ${_kos_cpu} on compile AND link is what picks the matching multilib, so the
# soft-float and 64-bit-divide helpers resolve.
string(JOIN " " _kos_common ${_kos_cpu} -ffunction-sections -fdata-sections)
set(CMAKE_C_FLAGS_INIT   "${_kos_common}")
set(CMAKE_CXX_FLAGS_INIT "${_kos_common}")
set(CMAKE_ASM_FLAGS_INIT "${_kos_common}")

# The Generic platform does not predefine the LINK_GROUP RESCAN feature the
# arch/kernel/chip archive cycle needs; GNU ld provides it via --start-group/--end-group.
foreach(_lang C CXX ASM)
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
endforeach()

# Bare-metal search rules: toolchain sysroot, never the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
