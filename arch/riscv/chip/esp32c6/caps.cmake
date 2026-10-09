# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The chip's capability declarations: its trace clock and its code window.
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

# The enforcing link's code window (esp32c6.ld). 128K, not 64K: the full-C++ opt-in folds
# .eh_frame and .gcc_except_table into it, and libstdc++ pushes code and rodata past 64K.
if(NOT DEFINED KICKOS_CODE_SIZE)
  set(KICKOS_CODE_SIZE 128K)
endif()
