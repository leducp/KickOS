# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# WeAct STM32F411CEU6 "Black Pill" (Cortex-M4F). 2nd board on the stm32f411 chip
# (same core as the Disco); per-board HW in boards/blackpill/include.
set(KICKOS_BOARD_ID "blackpill")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "stm32f411")
set(KICKOS_MCPU -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp)
