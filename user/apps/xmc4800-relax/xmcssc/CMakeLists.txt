# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800/USIC0-CH1 SSC consumer of the SPI class <kickos/driver/spi.h>, built BOTH WAYS from
# ONE source: the substitution property made concrete. The client body that opens a device and
# clocks transfers is identical in both; only who hands the client which capability differs,
# and that difference is the point (an endpoint cap and a window grant are not the same thing).
#
# DEFAULT (XMCSSC_LOCAL=OFF): SPI_BACKEND kickos_spi_proxy. The board service list brings the
# SSC service up before main, and the client reaches the bus over its endpoint. This is the
# shape the board's own default posture builds.
#
# XMCSSC_LOCAL=ON: SPI_BACKEND kickos_spi_xmcssc. The client owns the U0C1 window and the USIC0
# SR1 line and clocks the bus inline, with no service and no IPC. It needs a service list that
# does NOT start the SSC service, or the driver thread would already hold the window and the
# grant would be refused (one owner per block):
#   cmake -B <dir> --preset xmc4800-relax-st -DXMCSSC_LOCAL=ON \
#         -DKICKOS_SERVICE_LIST=kickos_services_xmc4800relax_console
#
# Diagnostic app: built only under KICKOS_ENABLE_SELFTEST (never a production image).
# Registered only for the xmc4800-relax board (chip-specific USIC bring-up) AND enforcement
# builds (the MMIO-grant seam is an M2 construct).

option(XMCSSC_LOCAL "Build xmcssc against the local SSC engine instead of the service proxy" OFF)

set(_xmcssc_backend kickos_spi_proxy)
if(XMCSSC_LOCAL)
  set(_xmcssc_backend kickos_spi_xmcssc)
endif()

kickos_add_diagnostic_app(xmcssc
  SOURCES     main.cc
  BOARD       ${KICKOS_BOARD}
  SPI_BACKEND ${_xmcssc_backend})
if(NOT TARGET xmcssc)
  return() # diagnostics off -> app not built
endif()

# kickos_xmcssc is linked in BOTH modes, for xmc_spi0_take_endpoint alone: its own engine copy
# is under private symbols, so it never competes with the backend selected above.
target_link_libraries(xmcssc PRIVATE kickos_xmcssc)

# Always defined, never merely absent: the app is built -Wundef -Werror, so an #if on an
# undefined name is a build failure rather than a silent false arm.
if(XMCSSC_LOCAL)
  target_compile_definitions(xmcssc PRIVATE KICKOS_SPI_LOCAL=1)
else()
  target_compile_definitions(xmcssc PRIVATE KICKOS_SPI_LOCAL=0)
endif()
message(STATUS "xmcssc: SPI backend = ${_xmcssc_backend}")

# No CTest gate: this app answers a HARDWARE question (an unprivileged SSC engine over the
# PMSA-granted USIC window, exercising the FEM=1 hardware CS-hold framing) and is validated by
# the operator on silicon, not by the in-env sim/QEMU.
