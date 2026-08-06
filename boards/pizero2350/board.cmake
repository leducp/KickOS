# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# Waveshare RP2350 Pi-Zero form factor, Cortex-M33 core (armv7m arch reused).
# Hard-float: the M33 has an FPv5-SP FPU; enable_fpu() + the FP-aware armv7m
# switch.S (s16-s31 keyed on EXC_RETURN bit 4) handle it, as on the M4F parts.
set(KICKOS_BOARD_ID "pizero2350")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "rp2350")
# The core and its FPU are the chip's (arch/arm/chip/rp2350/cpu.cmake). Only the float
# ABI is this board's: qemu-m33 is the same core and builds softfp.
set(KICKOS_MFLOAT_ABI hard)
