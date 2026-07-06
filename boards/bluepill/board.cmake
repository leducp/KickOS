# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# STM32F103 "Blue Pill", low-density clone / 10 KiB SRAM (Cortex-M3, no FPU).
set(KICKOS_BOARD_ID "bluepill")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "stm32f103")
set(KICKOS_MCPU -mcpu=cortex-m3 -mfloat-abi=soft)
