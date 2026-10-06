# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The RP2350 partition gate's row checks and register image (arch/arm/chip/rp2350/
# accessctrl_rows.h). rp2350_mmap.h is forced ahead of every include: a host build generates no
# RP2350 chip header.
kickos_add_unit_test(NAME rp2350gate
  SOURCES rp2350gate.cc
  INCLUDES "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/user/include")
target_compile_options(rp2350gate PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/rp2350_mmap.h")
