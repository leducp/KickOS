# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# The host sim: no chip backend, no cross CPU flags (it builds as a native ELF
# via cmake/toolchain-host.cmake, which never includes this file).
set(KICKOS_BOARD_ID "sim")
set(KICKOS_ARCH "sim")
set(KICKOS_CHIP "")
