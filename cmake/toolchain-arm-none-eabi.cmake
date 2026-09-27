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

kickos_toolchain_cross_programs(arm-none-eabi KICKOS_ARM_TOOLCHAIN_BIN)

# ${_kos_cpu} + -mthumb make the probe resolve THIS board's multilib, not the compiler's
# default.
include("${CMAKE_CURRENT_LIST_DIR}/cross_cxx_capability.cmake")
kickos_require_usable_cross_cxx("arm" "${CMAKE_CXX_COMPILER}"
  KICKOS_ARM_TOOLCHAIN_BIN
  "https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi.tar.xz"
  ${_kos_cpu} -mthumb)

# The selected Cortex-M/FPU/float-ABI flags identify the one newlib multilib this board links.
execute_process(COMMAND "${CMAKE_C_COMPILER}" ${_kos_cpu} -mthumb -print-multi-directory
                OUTPUT_VARIABLE _kos_multi OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
if(_kos_multi STREQUAL "thumb/v6-m/nofp")
  set(_kos_newlib_var KICKOS_NEWLIB_ARMV6M)
elseif(_kos_multi STREQUAL "thumb/v7-m/nofp")
  set(_kos_newlib_var KICKOS_NEWLIB_ARMV7M)
elseif(_kos_multi STREQUAL "thumb/v7e-m+fp/softfp")
  set(_kos_newlib_var KICKOS_NEWLIB_ARMV7EM_FP_SOFTFP)
elseif(_kos_multi STREQUAL "thumb/v7e-m+dp/softfp")
  set(_kos_newlib_var KICKOS_NEWLIB_ARMV7EM_DP_SOFTFP)
elseif(_kos_multi STREQUAL "thumb/v8-m.main+fp/softfp")
  set(_kos_newlib_var KICKOS_NEWLIB_ARMV8M_FP_SOFTFP)
elseif(_kos_multi STREQUAL "thumb/v8-m.main+fp/hard")
  set(_kos_newlib_var KICKOS_NEWLIB_ARMV8M_FP_HARD)
else()
  message(FATAL_ERROR "KickOS arm toolchain: no pinned newlib for multilib '${_kos_multi}'")
endif()
set(_kos_newlib_flavor "")
if(KICKOS_BOARD STREQUAL "microbit")
  set(_kos_newlib_flavor FLAVOR ${_kos_microbit_flavor})
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cross_newlib.cmake")
kickos_require_newlib("arm" "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}"
  ${_kos_newlib_var} static ${_kos_newlib_flavor} ${_kos_cpu} -mthumb)
if(KICKOS_BOARD STREQUAL "microbit")
  if(DEFINED KICKOS_MICROBIT_PACKAGE_NEWLIB
     AND NOT KICKOS_NEWLIB_FLAVOR STREQUAL KICKOS_MICROBIT_PACKAGE_NEWLIB)
    message(FATAL_ERROR
      "KickOS microbit: this KickOS package was built with ${KICKOS_MICROBIT_PACKAGE_NEWLIB} "
      "newlib, but ${_kos_newlib_var} selects ${KICKOS_NEWLIB_FLAVOR}. Provision armv6m with "
      "Conan -o \"&:flavor=${KICKOS_MICROBIT_PACKAGE_NEWLIB}\" in a fresh build directory, "
      "or build against a KickOS package built with the ${KICKOS_NEWLIB_FLAVOR} profile.")
  elseif(NOT KICKOS_NEWLIB_FLAVOR STREQUAL _kos_microbit_flavor)
    message(FATAL_ERROR
      "KickOS microbit: expected ${_kos_microbit_flavor} newlib, but "
      "${_kos_newlib_var} selects ${KICKOS_NEWLIB_FLAVOR}. Provision armv6m with "
      "Conan -o \"&:flavor=${_kos_microbit_flavor}\" in a fresh build directory. "
      "Set -DKICKOS_MICROBIT_FULL_NEWLIB=ON only to compare the full profile.")
  endif()
endif()

# -mthumb: Cortex-M is Thumb-only. -mno-unaligned-access: some parts (K64F) forbid
# unaligned/burst accesses across a RAM bank boundary (0x2000_0000, SRAM_L|SRAM_U), so the
# compiler must never emit one.
string(JOIN " " _kos_common ${_kos_cpu} -mthumb -mno-unaligned-access
       -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")
kickos_toolchain_newlib_flags()

kickos_toolchain_bare_metal_rules()
