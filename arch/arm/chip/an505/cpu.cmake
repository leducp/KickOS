# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU mps2-an505: Cortex-M33 with a single-precision FPv5.

kickos_arm_cpu(FLOAT softfp MCPU -mcpu=cortex-m33 -mfpu=fpv5-sp-d16)
