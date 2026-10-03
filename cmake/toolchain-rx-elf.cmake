# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS Renesas RX targets (rx-elf, GNU RX).
#
# RX72M needs Renesas's changes to GCC: -misa=v3 and -mdfpu (arch/rx/chip/rx72m/cpu.cmake) do
# not exist in upstream GCC's rx-elf. The KickOS toolchain's rx-elf family is the pinned set
# plus Renesas's changes ported onto it, carried as patches in conan/toolchain/patches.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR rx)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "rx72m")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board: rx72m")

kickos_toolchain_board_descriptor("rx")
kickos_toolchain_cpu_baseline("rx" "rx")
kickos_toolchain_export_baseline("${_kos_cpu}")

set(KICKOS_ARCH_FAMILY "${KICKOS_ARCH_FAMILY}" CACHE STRING "KickOS ISA family (arm|rx)")

# rx-elf spells every C name in the link with one more leading underscore, as KICKOS_LD_C_SYM in
# the chip script does.
set(KICKOS_C_SYMBOL_PREFIX "_" CACHE INTERNAL "What the link spells before a C name")

kickos_toolchain_package(rx-elf)

include("${CMAKE_CURRENT_LIST_DIR}/cross_newlib.cmake")
kickos_require_toolchain_newlib("rx" "${CMAKE_C_COMPILER}" static ${_kos_cpu})

# RX instructions are always little-endian; only *data* endianness is selectable and GNU RX
# defaults to little-endian, matching the MDE option word the linker script emits (spike
# sec.1, sec.6).
string(JOIN " " _kos_common ${_kos_cpu} -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")
kickos_toolchain_newlib_flags()

kickos_toolchain_bare_metal_rules()
