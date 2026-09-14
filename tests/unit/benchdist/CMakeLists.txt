# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Host arms for the distribution family's histogram.
#
# Header-only, so this compiles the SHIPPED bodies rather than a copy.
kickos_add_unit_test(NAME bench_dist
  SOURCES bench_dist.cc
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
