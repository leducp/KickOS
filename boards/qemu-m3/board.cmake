# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# QEMU mps2-an385 (Cortex-M3): the runnable SOFT-FLOAT armv7m target. The AN385
# image shares the AN386 memory map exactly, so this board needs no linker-script
# override -- the `mps2` chip default serves it unchanged. What it buys over `qemu`
# is the no-FPU path: the M3 has no CP10/CP11, so the FP context-switch and lazy-
# stacking branches are compiled out and the plain integer switch is what runs.
# That posture otherwise exists only on silicon-only boards (due, bluepill-c8).
set(KICKOS_BOARD_ID "qemu-m3")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "mps2")
set(KICKOS_MCPU -mcpu=cortex-m3 -mfloat-abi=soft)
