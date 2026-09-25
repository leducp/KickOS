# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS AArch64 targets (aarch64-none-elf, the Arm GNU Toolchain
# bare-metal AArch64 build: newlib, ships libstdc++/libsupc++).
#
# The family value "arm64" is SEPARATE from "arm", which is M-profile only: no Thumb, no
# -mfloat-abi (the AArch64 psABI has one FP ABI), a 64-bit pointer, and its own arch/arm64
# tree.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "qemu-arm64")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board: qemu-arm64")

kickos_toolchain_board_descriptor("arm64")
kickos_toolchain_cpu_baseline("arm64" "arm64")
kickos_toolchain_export_baseline("${_kos_cpu}")

set(KICKOS_ARCH_FAMILY "arm64" CACHE STRING "KickOS ISA family (arm|rx|xtensa|riscv|arm64)")

kickos_toolchain_cross_programs(aarch64-none-elf KICKOS_AARCH64_TOOLCHAIN_BIN)

# ${_kos_cpu} makes the probe resolve THIS board's multilib, not the compiler's default.
include("${CMAKE_CURRENT_LIST_DIR}/cross_cxx_capability.cmake")
kickos_require_usable_cross_cxx("arm64" "${CMAKE_CXX_COMPILER}"
  KICKOS_AARCH64_TOOLCHAIN_BIN
  "https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf.tar.xz"
  ${_kos_cpu})

include("${CMAKE_CURRENT_LIST_DIR}/cross_newlib.cmake")
kickos_require_dynreent_newlib("arm64" "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}"
  KICKOS_NEWLIB_AARCH64 ${_kos_cpu})

# The same ${_kos_cpu} on compile AND link picks the matching multilib (libgcc/newlib/
# libstdc++).
string(JOIN " " _kos_common ${_kos_cpu} -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")
kickos_toolchain_newlib_flags()

kickos_toolchain_bare_metal_rules()
