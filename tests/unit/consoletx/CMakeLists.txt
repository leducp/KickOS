# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The interrupt-masked window of console_tx_write, which any unprivileged thread can drive
# through kos_kconsole_write. kernel/init/console_tx.cc is compiled here as the real kernel
# translation unit (kickos_kernel's include path and definitions verbatim); tx_seam.cc
# answers the arch and console-ownership boundary. Host-only: the metric is bytes pushed per
# masked span, and no board can report that without an instrumented mask.
kickos_add_unit_test(NAME console_tx_masked_window
  SOURCES masked_window.cc tx_seam.cc
          "${PROJECT_SOURCE_DIR}/kernel/init/console_tx.cc"
  INCLUDES "$<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>"
  DEFINITIONS "$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>")
