# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The BADGE's semantics, over the real capability layer: where it is seated, that it is
# seated ONCE, and that a released slot never hands its last occupant's badge to the next
# entry. The packing itself is tests/unit/capreply's, which drives the header standalone.
kickos_add_unit_test(NAME cap_badge SOURCES cap_badge.cc
  LIBRARIES kickos_kseam)
