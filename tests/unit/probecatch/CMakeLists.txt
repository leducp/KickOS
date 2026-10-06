# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Which faults the ARMv7-M probe read swallows (arch/arm/armv7m/probe_catch.h).
kickos_add_unit_test(NAME probecatch
  SOURCES probecatch.cc
  INCLUDES "${PROJECT_SOURCE_DIR}")
