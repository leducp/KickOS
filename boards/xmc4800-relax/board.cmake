# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# Infineon XMC4800 Relax Kit (Cortex-M4F). Board is the Relax Kit; SoC is xmc4800.
set(KICKOS_BOARD_ID "xmc4800-relax")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "xmc4800")
