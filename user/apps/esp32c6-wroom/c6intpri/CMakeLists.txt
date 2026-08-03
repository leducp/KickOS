# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ESP32-C6 CPU-interrupt-controller identity probe: INTPRI @0x600C_5000 read through a
# 256 B PMP window while the kernel writes the undocumented 0x2000_1000 enable.
# Diagnostic app: built only under KICKOS_ENABLE_SELFTEST, which is also what compiles
# in the KOS_SYS_IRQ_UNMASK / KOS_SYS_IRQ_INJECT / KOS_SYS_GRANT_PROBE arms it needs.
kickos_add_diagnostic_app(c6intpri
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET c6intpri)
  return() # diagnostics off -> app not built
endif()

# No CTest gate: this answers a HARDWARE question and is validated by the operator on
# silicon, not by the in-env sim/QEMU.
