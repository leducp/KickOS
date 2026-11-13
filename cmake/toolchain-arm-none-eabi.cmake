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

# The board picks the arch; the *chip* picks the exact core + FPU. Both SAM3X8E (Cortex-M3, no
# FPU) and F411 (Cortex-M4F) resolve to the armv7m arch and need different -mcpu, so the flags
# key off the chip and not the arch.
kickos_toolchain_board_descriptor("arm")
kickos_toolchain_cpu_baseline("arm" "arm")

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
include("${CMAKE_CURRENT_LIST_DIR}/cross_newlib.cmake")
kickos_require_toolchain_newlib("arm" "${CMAKE_C_COMPILER}" ${_kos_cpu} -mthumb)

string(JOIN " " _kos_common ${_kos_cpu} -mthumb -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")

kickos_toolchain_bare_metal_rules()
