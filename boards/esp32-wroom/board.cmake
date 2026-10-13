# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# ESP32-WROOM-32 (ESP32, Xtensa LX6). The xtensa-esp32-elf toolchain fixes the core and the
# windowed ABI, so the board names no CPU flag.
set(KICKOS_BOARD_ID "esp32-wroom")
set(KICKOS_ARCH_FAMILY "xtensa")
set(KICKOS_ARCH "lx6")
set(KICKOS_CHIP "esp32")
