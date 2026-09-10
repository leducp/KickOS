# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Chip-family opt-in: this chip takes its shared bring-up from the RP2xxx unit, which
# arch/CMakeLists.txt appends to THIS CHIP's archive. Not an arch source: the RP2040 is
# armv6m and the RP2350 armv7m, so the two parts link no arch archive in common.

set(KICKOS_CHIP_FAMILY_DIR "${CMAKE_CURRENT_LIST_DIR}/../rp2xxx")
set(KICKOS_CHIP_FAMILY_SOURCES "${KICKOS_CHIP_FAMILY_DIR}/chip_rp2xxx.cc")
