# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU mps2-an385 (Cortex-M3, no FPU).
set(KICKOS_BOARD_ID "qemu-m3")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "mps2")
# mps2 serves three QEMU FPGA images with three different cores, so each board names its own.
set(KICKOS_MCPU -mcpu=cortex-m3)
set(KICKOS_MFLOAT_ABI soft)
