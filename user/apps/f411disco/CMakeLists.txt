# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# f411disco-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# f411disco), and the diagnostic posture is applied here so the app files never ask.

# The ungranted read must fault, which only an enforcing build can witness.
if(KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(f411spi)
endif()
