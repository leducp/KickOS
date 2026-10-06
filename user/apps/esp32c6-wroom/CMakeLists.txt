# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# esp32c6-wroom-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# esp32c6-wroom), and the diagnostic posture is applied here so the app files never ask.

# The ungranted write must fault, which only an enforcing build can witness.
if(KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(c6blink)
endif()

# The LP probe reads a kernel word and starts the LP core outside any partition; the TX idle
# witness drives the console's UART and pad.
if(NOT KICKOS_HAVE_MPU AND NOT KICKOS_AMP_OWN_IMAGE)
  add_subdirectory(c6lpprobe)
  add_subdirectory(c6txidle)
endif()
