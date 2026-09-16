# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# What forbids deriving a donee's effective priority from anything cheaper than the funnel
# itself. Host-only: the state is a donor raised while parked, which a running system holds
# for as long as it takes the donee to reach its next recompute.
kickos_add_unit_test(NAME prio_donor SOURCES donor_reprice.cc
  LIBRARIES kickos_kseam)
