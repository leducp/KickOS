# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# ESP32-WROOM-32 (ESP32, Xtensa LX6) is the first non-ARM board. Its Xtensa
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
