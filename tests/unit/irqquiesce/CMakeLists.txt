# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The interrupt entry against a concurrent teardown. kernel/irq/irq.cc is compiled here as the
# real kernel translation unit, and irq_seam.cc answers its whole undefined set. The
# notification layer is SEAMED and not compiled in: pulling notify.cc would pull the whole
# capability layer behind it, and nothing here is about what a raise does once it lands.
#
# AT TWO KERNEL CORES, which is the point and is why this gate compiles its own copy rather
# than joining the K-seam fixture: that library takes the preset's core count, and the sim
# preset drives one core, where the mechanism under test compiles to nothing.
kickos_add_unit_test(NAME irq_quiesce
  SOURCES quiesce.cc irq_seam.cc
          "${PROJECT_SOURCE_DIR}/kernel/irq/irq.cc"
          "${PROJECT_SOURCE_DIR}/kernel/sync/klock.cc"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1)
