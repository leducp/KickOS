# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# Raspberry Pi Pico (RP2040, Cortex-M0+, armv6m).
set(KICKOS_BOARD_ID "picopi")
set(KICKOS_ARCH "armv6m")
set(KICKOS_CHIP "rp2040")
