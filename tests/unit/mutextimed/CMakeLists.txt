# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The mutex lock's deadline over the real sync.cc, park.cc and time.cc: a try, an expiry and the
# waiting forms. Host-only of necessity: a grant, an owner's death and the expiry must reach one
# park in a chosen order, which no image can arrange.
#
# Needs kickos_kseam_time (tests/unit/parkdeath) and kickos_kseam_time_smp (tests/unit/semtimed)
# to exist before this directory is added.
kickos_add_unit_test(NAME mutex_timed SOURCES mutex_timed.cc
  LIBRARIES kickos_kseam_time)
kickos_add_unit_test(NAME mutex_timed_smp SOURCES mutex_timed.cc
  LIBRARIES kickos_kseam_time_smp
  TEST_PREFIX smp.)
