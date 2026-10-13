# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# console_tx_insert_line and the producer's own drain. kernel/init/console_tx.cc is compiled
# here as the real kernel translation unit, with the kernel's line router, and line_seam.cc
# answers the rest of its undefined set.
#
# AT ONE KERNEL CORE, and with the drain delivered by each arm rather than by a mask gap: the
# arms read a ring whose head, tail and re-entry flags move in an order the arm itself chose.
# tests/unit/consoletxsmp carries the two-core posture, where a drain runs on a core the
# producer's mask never reached.
kickos_add_unit_test(NAME console_tx_line
  SOURCES line_atomicity.cc producer_drain.cc amp_retry.cc line_seam.cc
          "${_cs}/ring_leaves.cc"
          "${PROJECT_SOURCE_DIR}/tests/unit/common/kpanic_seam.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/console_tx.cc"
          "${PROJECT_SOURCE_DIR}/kernel/irq/irq_route.cc"
  KERNEL
  DEFINITIONS KICKOS_LINE_STEP=1)
