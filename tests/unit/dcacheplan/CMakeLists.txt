# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# How one ARMv7-M data-cache maintenance call runs (arch/arm/armv7m/dcache_plan.h).
kickos_add_unit_test(NAME dcacheplan
  SOURCES dcacheplan.cc
  INCLUDES "${PROJECT_SOURCE_DIR}")
