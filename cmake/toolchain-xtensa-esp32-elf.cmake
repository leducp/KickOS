# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS ESP32 (Xtensa LX6) target -- the first non-ARM
# ISA. Sibling of toolchain-arm-none-eabi.cmake; the ISA-agnostic parts (the
# board-descriptor include, the LINK_GROUP RESCAN feature, the static-library
# try-compile, the bare-metal find rules) mirror that file.
#
# Unlike arm-none-eabi (one toolchain, -mcpu selects the core), the Xtensa core
# configuration is baked into the toolchain at build time, so the toolchain is
# chip-family-specific (xtensa-esp32-elf-* is the classic ESP32 LX6 overlay) and
# there is no per-board -mcpu. We use the toolchain's DEFAULT (windowed) ABI: the
# prebuilt esp32 multilib ships ONLY a windowed-ABI libgcc/libc, so windowed code
# links against it cleanly -- notably the 64-bit divide/modulo helpers
# (_udivdi3/_umoddi3) the ns<->cycle clock math pulls in. (The earlier call0 draft
# hit a hard wall here: `-mabi=call0 -print-multi-directory` still resolves to the
# windowed `esp32` multilib, so call0 code + windowed libgcc mismatched at link and
# would fault on HW. Windowed is the ecosystem-standard ABI and what ROM/BLE/WiFi
# blobs are built with.) The switch cost is a spill-all-windows context switch
# (arch/xtensa/lx6/switch.S) + the mandatory window over/underflow vectors
# (arch/xtensa/chip/esp32/startup.S).

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)

set(KICKOS_BOARD "esp32wroom" CACHE STRING "Target board: esp32wroom")

# The board descriptor is the single source of truth for arch/chip (mirrors the
# ARM toolchain). In-tree it is boards/<board>/board.cmake relative to the repo
# root (this file lives in <repo>/cmake); an installed single-board package ships
# the one board's descriptor beside this file.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/../boards/${KICKOS_BOARD}/board.cmake") # in-tree
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/board.cmake")
  set(KICKOS_BOARD "${KICKOS_BOARD_ID}" CACHE STRING "Target board" FORCE)
else()
  message(FATAL_ERROR "KickOS xtensa toolchain: no board descriptor for '${KICKOS_BOARD}'")
endif()

set(KICKOS_ARCH        "${KICKOS_ARCH}" CACHE STRING "KickOS arch backend selected by this toolchain")
set(KICKOS_ARCH_FAMILY "xtensa"         CACHE STRING "KickOS arch family (arm|xtensa)")

# The Espressif prebuilt toolchain is not on PATH by default (it lives under
# ~/.espressif, unlike apt's gcc-arm-none-eabi). Allow an override and search the
# known install location; a consumer on a different host sets KICKOS_XTENSA_BIN.
set(KICKOS_XTENSA_BIN
    "/home/leduc/.espressif/tools/xtensa-esp-elf/esp-16.1.0_20260609/xtensa-esp-elf/bin"
    CACHE PATH "Directory holding xtensa-esp32-elf-* programs")

find_program(CMAKE_C_COMPILER   xtensa-esp32-elf-gcc     HINTS "${KICKOS_XTENSA_BIN}" REQUIRED)
find_program(CMAKE_CXX_COMPILER xtensa-esp32-elf-g++     HINTS "${KICKOS_XTENSA_BIN}" REQUIRED)
find_program(CMAKE_ASM_COMPILER xtensa-esp32-elf-gcc     HINTS "${KICKOS_XTENSA_BIN}" REQUIRED)
find_program(CMAKE_OBJCOPY      xtensa-esp32-elf-objcopy HINTS "${KICKOS_XTENSA_BIN}" REQUIRED)
find_program(CMAKE_SIZE         xtensa-esp32-elf-size    HINTS "${KICKOS_XTENSA_BIN}")

# No linker script + startup during CMake's compiler probe (the board supplies
# them at the app-link step), so probe with a static library -- a step boundary
# must always configure standalone.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Windowed ABI = toolchain default: emit NO -mabi flag, so the correct (windowed)
# esp32 multilib is selected for compile AND link. -mlongcalls: let the assembler
# relax calls that exceed the +/-512 KB call range. -mtext-section-literals: keep
# each function's literal pool in .text so hand-written .S vectors carry their own
# literals (the GCC default splits literals into a section the bare-metal link
# would otherwise have to place). -ffunction/-fdata-sections + --gc-sections (at
# link) drop unreferenced code.
set(KICKOS_MCPU_FLAGS "" CACHE INTERNAL "Xtensa ABI/core baseline (windowed default)")

string(JOIN " " _kos_common -mlongcalls -mtext-section-literals
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

# Bare-metal search rules: toolchain sysroot, never the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
