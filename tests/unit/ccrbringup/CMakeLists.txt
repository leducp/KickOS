# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# CCR as the ARMv7-M bring-up leaves it, whatever the boot ROM left.
kickos_add_unit_test(NAME ccr_bringup
  SOURCES ccr_bringup.cc
  INCLUDES "${PROJECT_SOURCE_DIR}")
