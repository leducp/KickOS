# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# Arduino Due (AT91SAM3X8E, Cortex-M3, no FPU). EXPERIMENTAL: clock/console.
set(KICKOS_BOARD_ID "due")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "sam3x8e")
