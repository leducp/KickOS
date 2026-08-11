# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Allocation-policy gate for kernel/include/kickos/slotpool.h: the header is standalone
# (stdint.h only), so a host program instantiates the real template and measures how a
# create/destroy workload spreads its recycles -- the quantity that sets the ABA wrap
# distance. Host-only, so it registers on the sim build only, like the telemetry gates.
add_executable(slotpool_policy slotpool_policy.cc)
target_include_directories(slotpool_policy PRIVATE "${PROJECT_SOURCE_DIR}/kernel/include")
target_compile_options(slotpool_policy PRIVATE ${KICKOS_WARN_FLAGS})

add_test(NAME slotpool_policy COMMAND "$<TARGET_FILE:slotpool_policy>")
set_tests_properties(slotpool_policy PROPERTIES TIMEOUT 30 LABELS host)
