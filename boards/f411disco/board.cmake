# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# STM32F411E-DISCO (Cortex-M4F). Chip backend stm32f411, shared with blackpill;
# per-board HW is in boards/f411disco/include/kickos/board_config.h.
set(KICKOS_BOARD_ID "f411disco")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "stm32f411")
set(KICKOS_MCPU -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp)
