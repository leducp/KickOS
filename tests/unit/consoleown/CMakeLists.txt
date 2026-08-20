# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Ownership gate for kernel/init/console.cc: who may poke the UART, and when. The real
# translation unit is compiled in and given a recording arch seam, arch_irq_save included.
#
# It takes the kernel target's include path and definitions verbatim (KICKOS_CONSOLE_CHIP
# among them) but NOT its compile options: -ffreestanding describes an image with no host
# libc and this is a hosted program.
#
# HOST-ONLY: it defines the arch and console_tx seam names an image resolves from its own
# backend, and the injected flip needs a stub arch_irq_save.
kickos_add_unit_test(NAME console_ownership
  SOURCES console_ownership.cc "${PROJECT_SOURCE_DIR}/kernel/init/console.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)

# Both console translation units compiled together: kernel/init/console.cc (ownership) and
# kernel/init/console_tx.cc (the ring), so the publish sequence and the ring producer race
# each other rather than a stub. Include path, definitions and compile options as above.
#
# HOST-ONLY: the publish must land inside a specific mask gap of the writer it races, which
# needs an instrumented arch_irq_save/restore and no second thread.
kickos_add_unit_test(NAME console_publish_handoff
  SOURCES publish_handoff.cc publish_seam.cc
          "${PROJECT_SOURCE_DIR}/kernel/init/console.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/console_tx.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
