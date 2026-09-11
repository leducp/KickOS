# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Raspberry Pi RP2350: Cortex-M33 with a single-precision FPv5.

kickos_arm_cpu(FLOAT softfp MCPU -mcpu=cortex-m33 -mfpu=fpv5-sp-d16)
