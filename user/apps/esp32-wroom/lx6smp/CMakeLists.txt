# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ESP32 LX6 shared-kernel silicon probe: does S32C1I (Conditional Store Option) exclude
# between PRO_CPU and APP_CPU over internal SRAM, and does PRID differ across the two cores?
# Both are Xtensa configuration OPTIONS whose realisation no document on hand states, so the
# measurement is the source. Diagnostic app: built only under KICKOS_ENABLE_SELFTEST, and
# registered only for the esp32-wroom board (the APP_CPU launch is chip-specific).
kickos_add_diagnostic_app(lx6smp
  SOURCES main.cc app_cpu.S
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET lx6smp)
  return() # diagnostics off -> app not built
endif()

# kickos_core force-includes kickos/app.h and kickos/sys/init.h into every app TU
# (CMakeLists.txt), and the assembler chokes on their C declarations. The APP_CPU side
# needs no preprocessing at all, so it is assembled directly.
set_source_files_properties(app_cpu.S PROPERTIES COMPILE_OPTIONS "-x;assembler")

# No CTest gate: this answers a HARDWARE question and is validated by the operator on
# silicon, not by the in-env sim/QEMU. There is no ESP32 emulator in this tree.
