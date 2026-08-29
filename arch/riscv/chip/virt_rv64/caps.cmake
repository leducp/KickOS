# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trace-clock capability declaration (mirrors the mpu.cmake opt-in). The top CMakeLists
# includes this in its own scope, so a plain set (no PARENT_SCOPE) is what it reads.
# KICKOS_HAVE_TRACE_CLOCK is guarded with NOT DEFINED so a board/preset that pre-defined
# it on the command line still wins.
#
# The QEMU virt rv64 core implements the Zicntr counters, but this port ships no
# arch_trace_now, so the capability is 0 here and HAS_TRACE_CLOCK is unselected in
# arch/Kconfig to match. KICKOS_TRACE_ARCH is left at the top-level default: the ArchId
# enum in include/kickos/trace/record.h has no identifier for this backend yet.
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 0)
endif()
