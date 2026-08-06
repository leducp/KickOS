# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NXP i.MX RT1062: Cortex-M7 with a double-precision FPv5.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.

set(KICKOS_MCPU -mcpu=cortex-m7 -mfpu=fpv5-d16)

if(NOT DEFINED KICKOS_MFLOAT_ABI)
  set(KICKOS_MFLOAT_ABI softfp)
endif()
