# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# ST Nucleo-F302R8 (STM32F302R8, Cortex-M4F).
set(KICKOS_BOARD_ID "f302nucleo")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "stm32f302")
set(KICKOS_MCPU -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp)
