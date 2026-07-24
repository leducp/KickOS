# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# frdmk64f-only apps. The directory is the board gate (added only when
# KICKOS_BOARD is frdmk64f); each app keeps its own selftest/MPU gate.

# K64F GPIO/timer first driver over the MMIO grant (PIT + SYSMPU); also self-gates
# on KICKOS_ENABLE_SELFTEST in its own CMakeLists.
add_subdirectory(k64drv)

# K64F/DSPI0 unprivileged SPI driver -- the MMIO-grant seam is an M2 construct, so
# enforcement builds only; also self-gates on KICKOS_ENABLE_SELFTEST.
if(KICKOS_HAVE_MPU)
  add_subdirectory(k64dspi)
endif()

# K64F console handover demo -- self-gates on enforcement in its own CMakeLists.
add_subdirectory(k64console)
