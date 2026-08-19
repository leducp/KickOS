# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# rx72m-only apps. The directory is the board gate (added only when KICKOS_BOARD is
# rx72m); each app keeps its own selftest gate.

# RX72M GPIO blink: the RX MPU per-thread MMIO isolation reference. The MMIO-grant
# seam is an M2 construct, so enforcement builds only; also self-gates on
# KICKOS_ENABLE_SELFTEST.
if(KICKOS_HAVE_MPU)
  add_subdirectory(rxdrv)
endif()
