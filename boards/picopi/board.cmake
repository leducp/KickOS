# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# Raspberry Pi Pico (RP2040, Cortex-M0+, armv6m).
set(KICKOS_BOARD_ID "picopi")
set(KICKOS_ARCH "armv6m")
set(KICKOS_CHIP "rp2040")
set(KICKOS_MCPU -mcpu=cortex-m0plus -mfloat-abi=soft)
