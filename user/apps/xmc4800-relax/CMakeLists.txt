# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# xmc4800-relax-only apps. The directory is the board gate (added only when
# KICKOS_BOARD is xmc4800-relax); each app keeps its own selftest/MPU gate.

# USIC0-CH1 SSC (SPI) drivers/benches -- the MMIO-grant seam is an M2 construct, so
# enforcement builds only; each also self-gates on KICKOS_ENABLE_SELFTEST.
if(KICKOS_HAVE_MPU)
  add_subdirectory(xmcspi)
  add_subdirectory(xmccshold)
  add_subdirectory(xmcssc)
endif()

# XMC4800 console handover demo -- self-gates on enforcement in its own CMakeLists.
add_subdirectory(consoledemo)
