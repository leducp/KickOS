# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trace-clock / trace-arch capability declaration.
#
# The QEMU virt core implements the Zicntr counters, so the rv32imac `rdcycle`
# arch_trace_now is legal here. The capability is per chip, not per arch: the sibling
# esp32c6 traps on the same instruction. Under emulation the count is a timing estimate.
set(KICKOS_TRACE_ARCH 5)
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 1)
endif()
