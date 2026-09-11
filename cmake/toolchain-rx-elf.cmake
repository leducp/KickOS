# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS Renesas RX targets (rx-elf, GNU RX).
#
# RX72M needs the RENESAS GNURX build: -misa=v3 and -mdfpu (arch/rx/chip/rx72m/cpu.cmake) do
# not exist in upstream GCC's rx-elf, which is also why RX has no CI gate.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR rx)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "rx72m")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board: rx72m")

kickos_toolchain_board_descriptor("rx")
kickos_toolchain_cpu_baseline("rx" "rx")
kickos_toolchain_export_baseline("${_kos_cpu}")

set(KICKOS_ARCH_FAMILY "${KICKOS_ARCH_FAMILY}" CACHE STRING "KickOS ISA family (arm|rx)")

kickos_toolchain_cross_programs(rx-elf KICKOS_RX_TOOLCHAIN_BIN)

# RX instructions are always little-endian; only *data* endianness is selectable and GNU RX
# defaults to little-endian, matching the MDE option word the linker script emits (spike
# sec.1, sec.6).
string(JOIN " " _kos_common ${_kos_cpu} -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")

kickos_toolchain_bare_metal_rules()
