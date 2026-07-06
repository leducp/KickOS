# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# Arduino Due (AT91SAM3X8E, Cortex-M3, no FPU). EXPERIMENTAL: clock/console.
set(KICKOS_BOARD_ID "due")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "sam3x8e")
set(KICKOS_MCPU -mcpu=cortex-m3 -mfloat-abi=soft)
