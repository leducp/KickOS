# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800/USIC0-CH1 SSC bus-SERVICE client demo (M4.4 call/reply): drives the SSC bus
# service (brought up by the board service list) as a spawned unprivileged client over
# the chip-neutral kickos_spi_client wrapper, running loopback transfers with the
# hardware FEM=1 CS-hold framing. Diagnostic app: built only under
# KICKOS_ENABLE_SELFTEST (never a production image). Registered only for the
# xmc4800-relax board (chip-specific USIC bring-up) AND enforcement builds (the
# MMIO-grant seam is an M2 construct).
kickos_add_diagnostic_app(xmcssc
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET xmcssc)
  return() # diagnostics off -> app not built
endif()

# The SSC service is the exported kickos_xmcssc driver; the client speaks the
# chip-neutral kickos_spi_client wrapper over its call/reply endpoint.
target_link_libraries(xmcssc PRIVATE kickos_xmcssc kickos_spi_client)

# No CTest gate: this app answers a HARDWARE question (an unprivileged SSC driver over
# the PMSA-granted USIC window, exercising the FEM=1 hardware CS-hold framing) and is
# validated by the operator on silicon, not by the in-env sim/QEMU.
