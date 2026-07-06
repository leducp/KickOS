# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# QEMU mps2-an386 (Cortex-M4F): the runnable armv7m verification target.
set(KICKOS_BOARD_ID "qemu")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "mps2")
set(KICKOS_MCPU -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp)
