# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trace-clock capability declaration (mirrors the mpu.cmake opt-in). The top CMakeLists
# includes this in its own scope, so a plain set (no PARENT_SCOPE) is what it reads.
# KICKOS_HAVE_TRACE_CLOCK is guarded with NOT DEFINED so a board/preset that pre-defined
# it on the command line still wins.
#
# The Cortex-A53 has the architected generic timer (CNTPCT_EL0) and the PMU cycle
# counter, but the armv8a backend ships no arch_trace_now yet, so the capability is 0
# here and HAS_TRACE_CLOCK is unselected in arch/Kconfig to match.
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 0)
endif()
