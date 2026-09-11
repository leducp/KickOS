# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# console_tx_insert_line and the producer's own drain. kernel/init/console_tx.cc is compiled
# here as the real kernel translation unit and line_seam.cc answers its whole undefined set.
#
# AT ONE KERNEL CORE, and with the drain delivered by each arm rather than by a mask gap: the
# arms read a ring whose head, tail and re-entry flags move in an order the arm itself chose.
# tests/unit/consoletxsmp carries the two-core posture, where a drain runs on a core the
# producer's mask never reached.
kickos_add_unit_test(NAME console_tx_line
  SOURCES line_atomicity.cc producer_drain.cc line_seam.cc
          "${PROJECT_SOURCE_DIR}/kernel/init/console_tx.cc"
  KERNEL
  DEFINITIONS KICKOS_CONSOLE_TX_BARRIER=consoleline_publish_barrier)

# The publish barrier is the one point inside a ring copy the kernel translation unit calls
# out to, and kickos/console_tx.h opens it for a weakly-ordered core to inject a fence. Bound
# to the seam's hook here, which carries that fence and one seated call. The declaration has
# to reach console_tx.cc, whose include block this gate may not change, so it is forced in.
target_compile_options(console_tx_line PRIVATE
  -include "${CMAKE_CURRENT_SOURCE_DIR}/line_barrier.h")
