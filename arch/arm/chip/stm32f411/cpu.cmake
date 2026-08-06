# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# ST STM32F411: Cortex-M4F, shared by f411disco and blackpill.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.

set(KICKOS_MCPU -mcpu=cortex-m4 -mfpu=fpv4-sp-d16)

if(NOT DEFINED KICKOS_MFLOAT_ABI)
  set(KICKOS_MFLOAT_ABI softfp)
endif()
