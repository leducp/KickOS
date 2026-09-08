# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F/DSPI0 consumer of the SPI class <kickos/driver/spi.h> over the MMIO-grant seam: the real
# KickCAT-target SPI substrate, a DSPI master on SPI0 driven from an unprivileged thread. It
# touches no MMIO, no CS and no grant: which backend answers the class here is the image
# posture's (system/CMakeLists.txt), and in the board's default posture that is the proxy over
# the endpoint the board service list brought up.
# No CTest gate: it answers a HARDWARE question (an unprivileged DSPI engine over the
# AIPS-opened slot) and is validated by the operator on silicon against a real ESC (or a
# SOUT->SIN loopback jumper).
kickos_add_app_target(k64dspi main.cc CLASSES spi)
# The endpoint accessor is the DRIVER's, not the class backend's: this app names the service it
# takes a capability from, which no posture chooses for it. It resolved without this line only
# where the board's service list happened to pull the same archive in.
target_link_libraries(k64dspi PRIVATE kickos_k64dspi kickos)
kickos_emit_image(k64dspi)

# WHAT IS ON THE WIRE, which is the operator's to state and no part of the class selection.
# Default: the LAN9252 BYTE_TEST probe (EasyCAT shield on the Arduino header, GPIO CS on
# D9/PTC4). Set -DK64DSPI_LOOPBACK=ON to build the SOUT->SIN loopback self-test instead
# (shield off, PTD2 to PTD3 jumper in place).
option(K64DSPI_LOOPBACK "Build k64dspi as the SOUT->SIN loopback self-test instead of the LAN9252 probe" OFF)
if(K64DSPI_LOOPBACK)
  target_compile_definitions(k64dspi PRIVATE K64DSPI_LOOPBACK=1)
endif()
