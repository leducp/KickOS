# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trace-clock / trace-arch capability declaration (mirrors the mpu.cmake opt-in). The
# top CMakeLists includes this in its own scope, so a plain set (no PARENT_SCOPE) is
# what it reads. KICKOS_HAVE_TRACE_CLOCK is guarded with NOT DEFINED so a board/preset
# that pre-defined it on the command line still wins.
#
# The rv32imac arch_trace_now is `rdcycle`, and the ESP32-C6 HP core implements Zicntr
# nowhere in its CSR set, mcounteren included (C6 TRM v1.2 section 1.5.1). Reading one is an
# illegal instruction in MACHINE mode as well as U-mode, so the reader traps in the kernel,
# not merely in a thread. The counters the part does carry (the custom mpcer/mpcmr/mpccr
# block, and the CLINT MTIME low word the KICKOS_BENCH cycle source reads) would each need an
# arch_trace_now override to serve here. So the capability stays 0 and telemetry on this
# board is refused at configure.
set(KICKOS_TRACE_ARCH 5)
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 0)
endif()
