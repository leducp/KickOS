# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Chip-family opt-in: this chip's PLL bring-up, monotonic clock and USART console are the
# STM32 F0/F1/F3 bodies, and it answers them through its own family_map.h.
#
# The family translation unit is compiled into THIS chip's archive, never one of its own:
# the linker scripts select .data/.bss by archive:member, so a new archive would fall
# through to the app catch-all.

set(KICKOS_CHIP_FAMILY_DIR "${CMAKE_CURRENT_LIST_DIR}/../stm32f1f3")
set(KICKOS_CHIP_FAMILY_SOURCES "${KICKOS_CHIP_FAMILY_DIR}/chip_stm32f1f3.cc")
