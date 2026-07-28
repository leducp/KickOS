# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# QEMU mps2-an500 (Cortex-M7): the runnable M7 target. The AN500 image shares the
# AN386 memory map exactly, so this board needs no linker-script override -- the
# `mps2` chip default serves it unchanged, and the whole board is this descriptor.
# What it buys over `qemu` is the M7 pipeline and its 16 MPU regions (the M4 has 8),
# so the enforcement posture exercises a descriptor count no other runnable board
# reaches.
set(KICKOS_BOARD_ID "qemu-m7")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "mps2")
set(KICKOS_MCPU -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=softfp)
