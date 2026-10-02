# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS AArch64 targets: the KickOS toolchain's aarch64-none-elf family
# (docs/design-m10-toolchain.md), newlib and libstdc++.
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

kickos_toolchain_package(aarch64-none-elf)

include("${CMAKE_CURRENT_LIST_DIR}/cross_newlib.cmake")
kickos_require_toolchain_newlib("arm64" "${CMAKE_C_COMPILER}" dynamic ${_kos_cpu})

# The same ${_kos_cpu} on compile AND link picks the matching multilib (libgcc/newlib/
# libstdc++).
string(JOIN " " _kos_common ${_kos_cpu} -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")
kickos_toolchain_newlib_flags()

kickos_toolchain_bare_metal_rules()
