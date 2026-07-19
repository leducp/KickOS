# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the ARM cross
# toolchain file (pre-project, for -mcpu). Side-effect free: set only these.
#
# Waveshare RP2350 Pi-Zero form factor, Cortex-M33 core (armv7m arch reused).
# Hard-float: the M33 has an FPv5-SP FPU; enable_fpu() + the FP-aware armv7m
# switch.S (s16-s31 keyed on EXC_RETURN bit 4) handle it, as on the M4F parts.
set(KICKOS_BOARD_ID "pizero2350")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "rp2350")
set(KICKOS_MCPU -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard)
