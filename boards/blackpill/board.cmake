# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# WeAct STM32F411CEU6 "Black Pill" (Cortex-M4F). 2nd board on the stm32f411 chip
# (same core as the Disco); per-board HW in boards/blackpill/include.
set(KICKOS_BOARD_ID "blackpill")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "stm32f411")
