# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Cross toolchain for the KickOS ESP32 (Xtensa LX6) target.
#
# The Xtensa core configuration is baked into the toolchain at build time, so the toolchain is
# chip-family-specific (xtensa-esp32-elf-* is the classic ESP32 LX6 overlay) and there is no
# per-board -mcpu. We take the toolchain's DEFAULT (windowed) ABI: the prebuilt esp32 multilib
# ships ONLY a windowed-ABI libgcc/libc. `-mabi=call0 -print-multi-directory` still resolves to
# the windowed `esp32` multilib, so call0 code plus windowed libgcc mismatches at link and
# faults on HW.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "esp32-wroom")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board: esp32-wroom")

kickos_toolchain_board_descriptor("xtensa")

# No per-board CPU baseline to resolve: the core is fixed by the toolchain build.
set(_kos_cpu "")
kickos_toolchain_export_baseline("${_kos_cpu}")

set(KICKOS_ARCH_FAMILY "xtensa" CACHE STRING "KickOS arch family (arm|xtensa)")

kickos_toolchain_cross_programs(xtensa-esp32-elf KICKOS_XTENSA_BIN)

# Emit NO -mabi flag, so the windowed esp32 multilib is selected for compile AND link.
# -mlongcalls: let the assembler relax calls that exceed the +/-512 KB call range.
# -mtext-section-literals: keep each function's literal pool in .text so hand-written .S
# vectors carry their own literals.
#
# -mserialize-volatile IS PINNED. It brackets every volatile access with MEMW, and MEMW is the
# ordering this backend's MMIO sequences rest on: the APP_CPU release writes six registers in
# an order the part requires, and the TIMG counter is read through a shadow register that must
# be latched before it is read. Neither carries a barrier of its own. It is the toolchain
# default at esp-16.1.0, and pinned here so the next toolchain's default cannot decide it.
string(JOIN " " _kos_common -mlongcalls -mtext-section-literals -mserialize-volatile
       -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")

kickos_toolchain_bare_metal_rules()
