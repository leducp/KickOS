# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NXP MK64F: Cortex-M4F.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.

set(KICKOS_MCPU -mcpu=cortex-m4 -mfpu=fpv4-sp-d16)

if(NOT DEFINED KICKOS_MFLOAT_ABI)
  set(KICKOS_MFLOAT_ABI softfp)
endif()
