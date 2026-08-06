# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# QEMU mps2-an505 (Cortex-M33, armv8-m mainline): the runnable PMSAv8 target. The
# `mps2` chip backend is pure semihosting (no CMSDK registers), so it serves the
# AN505 unchanged; only the memory map differs. The AN505 boots Secure, so the
# image lives in the secure aliases; see boards/qemu-m33/mps2.ld.
set(KICKOS_BOARD_ID "qemu-m33")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "mps2")
# The one chip in the tree whose CORE is not a chip fact: arch/arm/chip/mps2 serves four
# QEMU FPGA images that declare four different cores, so it ships no cpu.cmake and each
# board states its own.
set(KICKOS_MCPU -mcpu=cortex-m33 -mfpu=fpv5-sp-d16)
set(KICKOS_MFLOAT_ABI softfp)
