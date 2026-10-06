# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# QEMU mps2-an505 (Cortex-M33, armv8-m mainline): the runnable PMSAv8 target.
set(KICKOS_BOARD_ID "qemu-m33")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "an505")
