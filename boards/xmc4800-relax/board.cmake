# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# Infineon XMC4800 Relax Kit (Cortex-M4F). Board is the Relax Kit; SoC is xmc4800.
set(KICKOS_BOARD_ID "xmc4800-relax")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "xmc4800")
set(KICKOS_MCPU -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp)
