# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# frdmk64f-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# frdmk64f), and the diagnostic posture is applied here so the app files never ask.

kickos_add_diagnostic_apps(k64drv)

# Each #errors without enforcement.
if(KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(k64dspi)
  add_subdirectory(k64console)
endif()
