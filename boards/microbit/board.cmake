# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# BBC micro:bit v1 (nRF51822, Cortex-M0, armv6m): the runnable armv6m QEMU target.
set(KICKOS_BOARD_ID "microbit")
set(KICKOS_ARCH "armv6m")
set(KICKOS_CHIP "nrf51")
