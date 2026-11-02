# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# ST Nucleo-F302R8 (STM32F302R8, Cortex-M4F).
set(KICKOS_BOARD_ID "f302nucleo")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "stm32f302")
set(KICKOS_BOARD_NEWLIB "nano")
# 64 KiB of flash: the kernel keeps sharing image 0's runtime, linked in one link with it.
set(KICKOS_BOARD_IMAGE_SPLIT OFF)
