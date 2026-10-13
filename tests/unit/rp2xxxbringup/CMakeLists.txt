# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The values both RP2xxx parts' bring-up computes: the system timer's tick divisor and a pin
# function's pad side.
kickos_add_unit_test(NAME rp2xxx_bringup
  SOURCES bringup_values.cc
  INCLUDES "${PROJECT_SOURCE_DIR}")
