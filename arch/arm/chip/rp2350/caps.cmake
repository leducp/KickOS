# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trace-clock / trace-arch capability declaration.
#
# RP2350 reuses the armv7m arch (Cortex-M33), but does NOT take the DWT CYCCNT
# arch_trace_now fallback: chip_rp2350.cc defines its own from the 1 MHz TIMER0.
set(KICKOS_TRACE_ARCH 1)
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 1)
endif()
