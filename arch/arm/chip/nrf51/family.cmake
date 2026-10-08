# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Semihosting opt-in: the console seams are the shared semihosting unit, compiled into THIS
# chip's archive.

set(KICKOS_CHIP_FAMILY_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../common")
set(KICKOS_CHIP_FAMILY_SOURCES "${KICKOS_CHIP_FAMILY_DIR}/semihost_console.cc")
