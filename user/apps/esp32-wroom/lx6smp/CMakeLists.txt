# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ESP32 LX6 shared-kernel silicon probe: does S32C1I (Conditional Store Option) exclude
# between PRO_CPU and APP_CPU over internal SRAM, and does PRID differ across the two cores?
# Both are Xtensa configuration OPTIONS whose realisation no document on hand states, so the
# measurement is the source.
# No CTest gate: it answers a HARDWARE question and there is no ESP32 emulator in this tree.
kickos_add_app_target(lx6smp main.cc app_cpu.S)
target_link_libraries(lx6smp PRIVATE kickos)
kickos_emit_image(lx6smp)

# kickos_core force-includes kickos/app.h and kickos/sys/init.h into every app TU
# (CMakeLists.txt), and the assembler chokes on their C declarations. The APP_CPU side
# needs no preprocessing at all, so it is assembled directly.
set_source_files_properties(app_cpu.S PROPERTIES COMPILE_OPTIONS "-x;assembler")
