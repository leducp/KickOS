# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trace-clock / trace-arch capability declaration.
#
# MK64F is armv7m (Cortex-M4): the DWT CYCCNT arch_trace_now fallback is the trace clock.
set(KICKOS_TRACE_ARCH 1)
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 1)
endif()
