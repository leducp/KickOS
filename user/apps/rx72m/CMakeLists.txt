# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# rx72m-only apps. The directory IS the board gate (added only when KICKOS_BOARD is rx72m),
# and the diagnostic posture is applied here so the app files never ask.

# The ungranted write must fault, which only an enforcing build can witness.
if(KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(rxdrv)
endif()
