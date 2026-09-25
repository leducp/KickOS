# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS RISC-V targets: the pinned RISCStar riscv32-none-elf cross
# compiler (newlib, multilib, soft float).
#
# The riscv32-none-elf triple also ships the RV64 multilib, so the rv64 boards resolve out of
# this same toolchain: it is the MULTILIB that names the XLEN here, never the triple.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "qemu-riscv")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board: qemu-riscv | qemu-riscv64 | esp32c6-wroom")

kickos_toolchain_board_descriptor("riscv")
kickos_toolchain_cpu_baseline("riscv" "riscv")
kickos_toolchain_export_baseline("${_kos_cpu}")

set(KICKOS_ARCH_FAMILY "riscv" CACHE STRING "KickOS ISA family (arm|rx|xtensa|riscv)")

kickos_toolchain_cross_programs(riscv32-none-elf KICKOS_RISCV_TOOLCHAIN_BIN)

# ${_kos_cpu} makes the probe resolve THIS board's multilib, not the compiler's default.
include("${CMAKE_CURRENT_LIST_DIR}/cross_cxx_capability.cmake")
kickos_require_usable_cross_cxx("riscv" "${CMAKE_CXX_COMPILER}"
  KICKOS_RISCV_TOOLCHAIN_BIN
  "https://releases.riscstar.com/toolchain/16.1-r1/riscstar-toolchain-16.1-r1-x86_64-riscv32-none-elf.tar.xz"
  ${_kos_cpu})

if(KICKOS_ARCH STREQUAL "rv64imac")
  include("${CMAKE_CURRENT_LIST_DIR}/cross_newlib.cmake")
  kickos_require_dynreent_newlib("riscv" "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}"
    KICKOS_NEWLIB_RV64IMAC_LP64 ${_kos_cpu})
endif()

# The same ${_kos_cpu} on compile AND link is what picks the matching multilib, so the
# soft-float and 64-bit-divide helpers resolve.
string(JOIN " " _kos_common ${_kos_cpu} -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")
if(KICKOS_ARCH STREQUAL "rv64imac")
  kickos_toolchain_newlib_flags()
endif()

kickos_toolchain_bare_metal_rules()
