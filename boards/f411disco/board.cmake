# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# STM32F411E-DISCO (Cortex-M4F). Chip backend stm32f411, shared with blackpill; the two
# differ in wiring facts (crystal, diag LED), which each states in its own
# include/kickos/board_wiring.h. Their defconfigs differ only in CONFIG_BOARD_*.
set(KICKOS_BOARD_ID "f411disco")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "stm32f411")
