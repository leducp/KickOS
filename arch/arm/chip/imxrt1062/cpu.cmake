# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NXP i.MX RT1062: Cortex-M7 with a double-precision FPv5.

kickos_arm_cpu(FLOAT softfp MCPU -mcpu=cortex-m7 -mfpu=fpv5-d16)
