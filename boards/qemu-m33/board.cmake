# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# QEMU mps2-an505 (Cortex-M33, armv8-m mainline): the runnable PMSAv8 target. The
# `mps2` chip backend is pure semihosting (no CMSDK registers), so it serves the
# AN505 unchanged; only the memory map differs (the AN505 boots Secure, so the
# image lives in the secure aliases -- boards/qemu-m33/mps2.ld).
set(KICKOS_BOARD_ID "qemu-m33")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "mps2")
set(KICKOS_MCPU -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=softfp)
