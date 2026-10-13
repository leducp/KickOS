# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Waveshare RP2350 Pi-Zero form factor (Cortex-M33 with FPv5-SP FPU, armv7m arch).
set(KICKOS_BOARD_ID "pizero2350")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "rp2350")
# The float ABI is this board's; qemu-m33 is the same core and builds softfp.
set(KICKOS_MFLOAT_ABI hard)
