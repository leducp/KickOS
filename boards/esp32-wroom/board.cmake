# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the Xtensa cross
# toolchain file (pre-project, for the ABI baseline). Side-effect free: set only
# these.
#
# ESP32-WROOM-32 (ESP32-D0WDQ6, Xtensa LX6) -- the first non-ARM board. The Xtensa
# core config is baked into the xtensa-esp32-elf toolchain, so unlike ARM there is
# no -mcpu selection here: the ABI baseline is fixed by the toolchain file. The
# windowed ABI (the toolchain default) is used so the prebuilt esp32 libgcc/libc
# multilib links cleanly (see cmake/toolchain-xtensa-esp32-elf.cmake).
set(KICKOS_BOARD_ID "esp32-wroom")
set(KICKOS_ARCH_FAMILY "xtensa")
set(KICKOS_ARCH "lx6")
set(KICKOS_CHIP "esp32")
# No KICKOS_MCPU: the fixed-core Xtensa toolchain owns the ABI/ISA flags. Family is
# set explicitly (like rx) since it is not derivable from arch=lx6; the ARM boards
# omit it and the loader derives family=arm from the armv* arch.
