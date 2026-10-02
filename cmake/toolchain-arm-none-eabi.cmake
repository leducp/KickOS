# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS ARM Cortex-M targets (arm-none-eabi).
#
# The KickOS toolchain's arm-none-eabi family (docs/design-m10-toolchain.md): GCC's rmprofile
# multilibs, each with a full and a nano newlib and libstdc++.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "frdmk64f")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board (see boards/)")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KICKOS_BOARD)

# The board picks the arch; the *chip* picks the exact core + FPU. Both F103 (Cortex-M3, no
# FPU) and F411 (Cortex-M4F) resolve to the armv7m arch and need different -mcpu, so the flags
# key off the chip and not the arch.
kickos_toolchain_board_descriptor("arm")
kickos_toolchain_cpu_baseline("arm" "arm")

# After the descriptor: an installed package's descriptor states KICKOS_MICROBIT_PACKAGE_NEWLIB,
# the profile its libraries were built with. They size per-thread newlib state by that
# profile's struct _reent, so the package links no other.
if(KICKOS_BOARD STREQUAL "microbit")
  set(_kos_microbit_full_default OFF)
  if(KICKOS_MICROBIT_PACKAGE_NEWLIB STREQUAL "full")
    set(_kos_microbit_full_default ON)
  endif()
  set(KICKOS_MICROBIT_FULL_NEWLIB ${_kos_microbit_full_default} CACHE BOOL
      "Use full newlib instead of microbit's default nano profile")
  list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KICKOS_MICROBIT_FULL_NEWLIB)
  if(KICKOS_MICROBIT_FULL_NEWLIB)
    set(_kos_microbit_flavor full)
  else()
    set(_kos_microbit_flavor nano)
  endif()
  if(DEFINED KICKOS_MICROBIT_PACKAGE_NEWLIB
     AND NOT _kos_microbit_flavor STREQUAL KICKOS_MICROBIT_PACKAGE_NEWLIB)
    message(FATAL_ERROR
      "KickOS microbit: this KickOS package was built with ${KICKOS_MICROBIT_PACKAGE_NEWLIB} "
      "newlib, but KICKOS_MICROBIT_FULL_NEWLIB=${KICKOS_MICROBIT_FULL_NEWLIB} asks for "
      "${_kos_microbit_flavor}. Leave it unset in a fresh build directory, or build against "
      "a KickOS package built with the ${_kos_microbit_flavor} profile.")
  endif()
endif()

if(NOT DEFINED KICKOS_MFLOAT_ABI)
  message(FATAL_ERROR "KickOS arm toolchain: board '${KICKOS_BOARD}' states no float ABI; "
    "boards/${KICKOS_BOARD}/board.cmake or arch/arm/chip/${KICKOS_CHIP}/cpu.cmake must set "
    "KICKOS_MFLOAT_ABI")
endif()
list(APPEND _kos_cpu -mfloat-abi=${KICKOS_MFLOAT_ABI})

kickos_toolchain_export_baseline("${_kos_cpu}")

kickos_toolchain_package(arm-none-eabi)

# A board whose flags name no rmprofile multilib would link the compiler's default, Arm-state
# libraries.
execute_process(COMMAND "${CMAKE_C_COMPILER}" ${_kos_cpu} -mthumb -print-multi-directory
                OUTPUT_VARIABLE _kos_multi OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
if(NOT _kos_multi MATCHES "^thumb/")
  message(FATAL_ERROR "KickOS arm toolchain: '${_kos_cpu} -mthumb' selects multilib "
    "'${_kos_multi}', no Cortex-M one")
endif()
set(_kos_newlib_flavor "")
if(KICKOS_BOARD STREQUAL "microbit")
  set(_kos_newlib_flavor FLAVOR ${_kos_microbit_flavor})
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cross_newlib.cmake")
kickos_require_toolchain_newlib("arm" "${CMAKE_C_COMPILER}" static ${_kos_newlib_flavor}
  ${_kos_cpu} -mthumb)

# -mthumb: Cortex-M is Thumb-only. -mno-unaligned-access: some parts (K64F) forbid
# unaligned/burst accesses across a RAM bank boundary (0x2000_0000, SRAM_L|SRAM_U), so the
# compiler must never emit one.
string(JOIN " " _kos_common ${_kos_cpu} -mthumb -mno-unaligned-access
       -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")
kickos_toolchain_newlib_flags()

kickos_toolchain_bare_metal_rules()
