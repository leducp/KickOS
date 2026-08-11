# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F/DSPI0 consumer of the SPI class <kickos/driver/spi.h> over the MMIO-grant seam: the real
# KickCAT-target SPI substrate, a DSPI master on SPI0 driven from an unprivileged thread.
# SPI_BACKEND is the proxy, so the client reaches the bus over the service endpoint the board
# service list brought up. Diagnostic app: built only under KICKOS_ENABLE_SELFTEST (never a
# production image). Registered only for the frdmk64f board (chip-specific DSPI/PORTD/AIPS
# bring-up) AND enforcement builds (the MMIO-grant seam is an M2 construct).
kickos_add_diagnostic_app(k64dspi
  SOURCES     main.cc
  BOARD       ${KICKOS_BOARD}
  SPI_BACKEND kickos_spi_proxy)
if(NOT TARGET k64dspi)
  return() # diagnostics off -> app not built
endif()

# kickos_k64dspi is linked for k64dspi_take_endpoint alone: its own engine copy is under private
# symbols, so it never competes with the backend selected above.
target_link_libraries(k64dspi PRIVATE kickos_k64dspi)

# Default: the LAN9252 BYTE_TEST probe (EasyCAT shield on the Arduino header, GPIO CS on
# D9/PTC4). Set -DK64DSPI_LOOPBACK=ON to build the SOUT->SIN loopback self-test instead
# (shield off, PTD2<->PTD3 jumper in place).
option(K64DSPI_LOOPBACK "Build k64dspi as the SOUT->SIN loopback self-test instead of the LAN9252 probe" OFF)
if(K64DSPI_LOOPBACK)
  target_compile_definitions(k64dspi PRIVATE K64DSPI_LOOPBACK=1)
endif()

# No CTest gate: this app answers a HARDWARE question (an unprivileged DSPI engine over the
# AIPS-opened slot) and is validated by the operator on silicon against a real ESC (or a
# SOUT->SIN loopback jumper), not by the in-env sim/QEMU.
