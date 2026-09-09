# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Allocation-policy gate for kernel/include/kickos/slotpool.h: the header is standalone
# (stdint.h only), so a host program instantiates the real template and measures how a
# create/destroy workload spreads its recycles; the quantity that sets the ABA wrap
# distance. Host-only, so it registers on the sim build only, like the telemetry gates.
kickos_add_unit_test(NAME slotpool_policy
  SOURCES slotpool_policy.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/include")

# at() and live() take an INDEX, and both refuse one outside [0, N). Comparing the returned
# pointer cannot witness that refusal: drop the bound and `used_[-1]` reads the byte below the
# array, which on this layout is zero, so `not used_[-1]` refuses the slot BY ACCIDENT and the
# arm still passes. UBSan's bounds check is what turns the read itself into the property, which
# is also the harm: an out-of-range index here is a kernel-side out-of-bounds read whatever the
# pointer that comes back. Optional, on kstring_words' precedent: a host toolchain without it
# still runs every pointer check.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=bounds -fno-sanitize-recover=bounds")
check_cxx_source_compiles("int main(){return 0;}" KICKOS_HAVE_UBSAN_BOUNDS)
unset(CMAKE_REQUIRED_FLAGS)
if(KICKOS_HAVE_UBSAN_BOUNDS)
  target_compile_options(slotpool_policy PRIVATE -fsanitize=bounds -fno-sanitize-recover=bounds)
  target_link_options(slotpool_policy PRIVATE -fsanitize=bounds -fno-sanitize-recover=bounds)
else()
  message(STATUS "KickOS: no UBSan bounds check; slotpool_policy tests pointers only")
endif()
