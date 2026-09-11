# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS ARM Cortex-M targets (arm-none-eabi).
#
# The compiler this file wants is the official Arm GNU Toolchain (newlib-based, ships
# libstdc++/libsupc++): the full-C++ opt-in needs newlib's full libstdc++, which Debian's
# picolibc-based apt toolchain cannot provide. The capability check after the finds is what
# ENFORCES that, on whatever gets resolved (hint, PATH, or -D).

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "frdmk64f")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board (see boards/)")

# The board picks the arch; the *chip* picks the exact core + FPU. Both F103 (Cortex-M3, no
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

kickos_toolchain_cross_programs(arm-none-eabi KICKOS_ARM_TOOLCHAIN_BIN)

# ${_kos_cpu} + -mthumb make the probe resolve THIS board's multilib, not the compiler's
# default.
include("${CMAKE_CURRENT_LIST_DIR}/cross_cxx_capability.cmake")
kickos_require_usable_cross_cxx("arm" "${CMAKE_CXX_COMPILER}"
  KICKOS_ARM_TOOLCHAIN_BIN
  "https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi.tar.xz"
  ${_kos_cpu} -mthumb)

# -mthumb: Cortex-M is Thumb-only. -mno-unaligned-access: some parts (K64F) forbid
# unaligned/burst accesses across a RAM bank boundary (0x2000_0000, SRAM_L|SRAM_U), so the
# compiler must never emit one.
string(JOIN " " _kos_common ${_kos_cpu} -mthumb -mno-unaligned-access
       -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")

kickos_toolchain_bare_metal_rules()
