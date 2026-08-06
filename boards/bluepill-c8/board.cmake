# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# Genuine STM32F103C8 "Blue Pill", 20 KiB SRAM (Cortex-M3, no FPU). Same chip
# backend and linker script as the chip default (arch/arm/chip/stm32f103/stm32f103.ld).
set(KICKOS_BOARD_ID "bluepill-c8")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "stm32f103")
