# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# frdmk64f-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# frdmk64f), and the diagnostic posture is applied here so the app files never ask.

# K64F GPIO/timer first driver over the MMIO grant (PIT + SYSMPU).
kickos_add_diagnostic_apps(k64drv)

# K64F/DSPI0 unprivileged SPI driver: the MMIO-grant seam exists only under enforcement.
if(KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(k64dspi)
endif()

# K64F console handover demo. A real user-facing app, not a diagnostic. Enforcement builds
# only: the memory-fault reclaim trigger needs SYSMPU, and the handover is otherwise
# coarse-AIPS FUNCTIONAL.
if(KICKOS_HAVE_MPU)
  add_subdirectory(k64console)
endif()
