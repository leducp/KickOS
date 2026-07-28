# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# QEMU mps2-an385 (Cortex-M3): the runnable SOFT-FLOAT armv7m target. No CP10/CP11,
# so the FP context-switch and lazy-stacking branches are compiled out. Shares the
# AN386 memory map, so the `mps2` chip linker script serves it unchanged.
set(KICKOS_BOARD_ID "qemu-m3")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "mps2")
set(KICKOS_MCPU -mcpu=cortex-m3 -mfloat-abi=soft)
