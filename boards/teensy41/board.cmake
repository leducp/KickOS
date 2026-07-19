# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# PJRC Teensy 4.1 (NXP i.MX RT1062, Cortex-M7 with double-precision FPU).
set(KICKOS_BOARD_ID "teensy41")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "imxrt1062")
set(KICKOS_MCPU -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=softfp)
