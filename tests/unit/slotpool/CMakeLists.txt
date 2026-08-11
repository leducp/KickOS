# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Allocation-policy gate for kernel/include/kickos/slotpool.h: the header is standalone
# (stdint.h only), so a host program instantiates the real template and measures how a
# create/destroy workload spreads its recycles -- the quantity that sets the ABA wrap
# distance. Host-only, so it registers on the sim build only, like the telemetry gates.
kickos_add_unit_test(NAME slotpool_policy
  SOURCES slotpool_policy.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/include")
