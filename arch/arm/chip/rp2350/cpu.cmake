# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Raspberry Pi RP2350: Cortex-M33 with a single-precision FPv5.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.

set(KICKOS_MCPU -mcpu=cortex-m33 -mfpu=fpv5-sp-d16)

if(NOT DEFINED KICKOS_MFLOAT_ABI)
  set(KICKOS_MFLOAT_ABI softfp)
endif()
