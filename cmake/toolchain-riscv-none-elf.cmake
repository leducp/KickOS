# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS RISC-V targets: the KickOS toolchain's riscv64-none-elf family
# (docs/design-m10-toolchain.md), newlib and libstdc++, soft float.
#
# The one compiler carries the rv32 multilib beside its rv64 default, so every RISC-V board
# resolves out of it: it is the MULTILIB that names the XLEN here, never the triple.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "qemu-riscv")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board: qemu-riscv | qemu-riscv64 | esp32c6-wroom")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KICKOS_BOARD)

kickos_toolchain_board_descriptor("riscv")
kickos_toolchain_cpu_baseline("riscv" "riscv")
kickos_toolchain_export_baseline("${_kos_cpu}")

set(KICKOS_ARCH_FAMILY "riscv" CACHE STRING "KickOS ISA family (arm|rx|xtensa|riscv)")

kickos_toolchain_package(riscv64-none-elf)

include("${CMAKE_CURRENT_LIST_DIR}/cross_newlib.cmake")
set(_kos_rv_reent static)
if(KICKOS_ARCH STREQUAL "rv64imac")
  set(_kos_rv_reent dynamic)
endif()
kickos_require_toolchain_newlib("riscv" "${CMAKE_C_COMPILER}" ${_kos_rv_reent} ${_kos_cpu})

# The same ${_kos_cpu} on compile AND link is what picks the matching multilib, so the
# soft-float and 64-bit-divide helpers resolve.
string(JOIN " " _kos_common ${_kos_cpu} -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")
kickos_toolchain_newlib_flags()

kickos_toolchain_bare_metal_rules()
