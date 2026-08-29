# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trace-clock capability declaration.
#
# The Cortex-A53 has the architected generic timer (CNTPCT_EL0) and the PMU cycle
# counter, but the armv8a backend ships no arch_trace_now yet, so the capability is 0
# here and HAS_TRACE_CLOCK is unselected in arch/Kconfig to match.
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 0)
endif()
