# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F/DSPI0 unprivileged userspace SPI driver over the MMIO-grant seam (task #9
# Stage 3): the real KickCAT-target SPI substrate -- a DSPI master on SPI0 running
# a SOUT->SIN loopback from an unprivileged thread over the AIPS-opened slot.
# Diagnostic app: built only under KICKOS_ENABLE_SELFTEST (never a production
# image). Registered only for the frdmk64f board (chip-specific DSPI/PORTD/AIPS
# bring-up) AND enforcement builds (the MMIO-grant seam is an M2 construct).
kickos_add_diagnostic_app(k64dspi
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET k64dspi)
  return() # diagnostics off -> app not built
endif()

# The DSPI0 transport is the exported kickos_k64dspi driver, not a local copy.
target_link_libraries(k64dspi PRIVATE kickos_k64dspi)

# Default: the LAN9252 BYTE_TEST probe (EasyCAT shield on the Arduino header, HW PCS0
# on D9/PTC4). Set -DK64DSPI_LOOPBACK=ON to build the SOUT->SIN loopback self-test
# instead (shield off, PTD2<->PTD3 jumper in place, HW PCS0 on D10/PTD0).
option(K64DSPI_LOOPBACK "Build k64dspi as the SOUT->SIN loopback self-test instead of the LAN9252 probe" OFF)
if(K64DSPI_LOOPBACK)
  target_compile_definitions(k64dspi PRIVATE K64DSPI_LOOPBACK=1)
endif()

# No CTest gate: this app answers a HARDWARE question (an unprivileged DSPI driver
# over the AIPS-opened slot) and is validated by the operator on silicon against a
# real ESC (or a SOUT->SIN loopback jumper), not by the in-env sim/QEMU.
