# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and, on MCU, by the ARM
# cross toolchain file (pre-project, for the -mcpu baseline). Keep it side-effect
# free -- set only these variables.
#
# The host sim: no chip backend, no cross CPU flags (it builds as a native ELF
# via cmake/toolchain-host.cmake, which never includes this file).
set(KICKOS_BOARD_ID "sim")
set(KICKOS_ARCH "sim")
set(KICKOS_CHIP "")
