# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trace-clock / trace-arch capability declaration (mirrors the mpu.cmake opt-in). The
# top CMakeLists includes this in its own scope, so a plain set (no PARENT_SCOPE) is
# what it reads. KICKOS_HAVE_TRACE_CLOCK is guarded with NOT DEFINED so a board/preset
# that pre-defined it on the command line still wins.
#
# RP2040 is armv6m (Cortex-M0+, no DWT) but ships its own arch_trace_now, so it lifts
# the armv6m arch default (no trace clock).
set(KICKOS_TRACE_ARCH 2)
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 1)
endif()
