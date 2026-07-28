# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# QEMU mps2-an500 (Cortex-M7): the runnable M7 target, with 16 MPU regions where
# the M4 has 8. Shares the AN386 memory map, so the `mps2` chip linker script
# serves it unchanged; the whole board is this descriptor.
set(KICKOS_BOARD_ID "qemu-m7")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "mps2")
set(KICKOS_MCPU -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=softfp)
