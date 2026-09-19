# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Pins arch/riscv/chip/esp32c6/mtime_conv.h's reciprocal-multiply mtime_ns_to_ticks
# against the plain "ns * 4ull / 25ull" it replaces (M8.11: the plain form lowers to a
# software __udivdi3 call inside arch_timer_arm's masked window on rv32, and its cost
# grows with ns, i.e. with uptime). Header has no chip register dependencies, so it
# compiles unmodified on the host.
kickos_add_unit_test(NAME mtimeconv
  SOURCES mtimeconv.cc
  INCLUDES "${PROJECT_SOURCE_DIR}")
