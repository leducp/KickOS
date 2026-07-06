# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# BBC micro:bit v1 (nRF51822, Cortex-M0, armv6m): the runnable armv6m QEMU target.
set(KICKOS_BOARD_ID "microbit")
set(KICKOS_ARCH "armv6m")
set(KICKOS_CHIP "nrf51")
set(KICKOS_MCPU -mcpu=cortex-m0 -mfloat-abi=soft)
