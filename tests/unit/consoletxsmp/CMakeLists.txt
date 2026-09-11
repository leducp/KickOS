# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console ring's tail against a drain on another core. kernel/init/console_tx.cc is
# compiled here as the real kernel translation unit and tx_smp_seam.cc answers its whole
# undefined set.
#
# AT TWO KERNEL CORES, which is the point and is why this gate compiles its own copy rather
# than joining tests/unit/consoletx: that fixture runs the drain in the producer's own mask
# gap, which is the single-core machine, and no arm in it can put a drain on a core the
# producer's mask never reached.
kickos_add_unit_test(NAME console_tx_smp
  SOURCES tail_writers.cc tx_smp_seam.cc
          "${PROJECT_SOURCE_DIR}/kernel/init/console_tx.cc"
          "${PROJECT_SOURCE_DIR}/kernel/sync/klock.cc"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1)
