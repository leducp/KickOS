# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The BADGE's semantics, over the real capability layer: where it is seated, that it is
# seated ONCE, and that a released slot never hands its last occupant's badge to the next
# entry. The packing itself is tests/unit/captable's, which drives the header standalone.
kickos_add_unit_test(NAME cap_badge SOURCES cap_badge.cc reply_probe.cc
  LIBRARIES kickos_kseam)

# A task watch kept across a generation wrap of its notification's slot, refused at the raise.
kickos_add_unit_test(NAME task_stale_watch SOURCES stale_watch.cc
  LIBRARIES kickos_kseam)
