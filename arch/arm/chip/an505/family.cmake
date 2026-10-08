# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Chip-family opt-in: this chip takes the MPS2 backend, reset table and semihosting console,
# compiled into THIS chip's archive.

set(KICKOS_CHIP_FAMILY_DIR "${CMAKE_CURRENT_LIST_DIR}/../mps2")
set(KICKOS_CHIP_FAMILY_SOURCES "${KICKOS_CHIP_FAMILY_DIR}/chip_mps2.cc" "${KICKOS_CHIP_FAMILY_DIR}/startup.S"
    "${CMAKE_CURRENT_LIST_DIR}/../../../common/semihost_console.cc")
