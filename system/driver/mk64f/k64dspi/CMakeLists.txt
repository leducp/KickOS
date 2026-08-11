# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# TWO TARGETS OVER ONE ENGINE, and the split is the driver-model ruling.
#
# kickos_spi_k64dspi is the DSPI backend of the SPI class <kickos/driver/spi.h>: the public
# kos_spi_* symbols, for a consumer that owns the DSPI0 window and clocks the bus inline. It is
# what a consumer target names as SPI_BACKEND (cmake/kickos.cmake). Deliberately NOT a link
# dependency of the service below, so it never enters a board's service-list closure or the
# shared kickos link group: which backend an executable gets belongs to that executable.
#
# kickos_k64dspi is the SERVICE: the bring-up shim, the endpoint, the driver thread, and its OWN
# private copy of the same engine. An image hosting this service may also hold a consumer that
# reached the public class names through kickos_spi_proxy, and one ELF has one meaning per
# symbol, so the service's four class symbols are RENAMED below. A fifth class call that
# forgets its rename here is reported as a duplicate symbol ONLY where another definer of that
# public name is in the same link; forgetting the rename on ALL FOUR is not reported at all,
# and tests/check_class_backend.sh is what catches that. See k64dspi.cc.
#
# The DSPI RX-FIFO fill-level read is shared register logic, so both targets link the
# kickos_class_mk64f leaf rather than carrying a local copy.

kickos_add_driver(spi_k64dspi SOURCES spi_dspi.cc CLASS kickos_class_mk64f)

kickos_add_driver(k64dspi SOURCES k64dspi.cc spi_dspi.cc CLASS kickos_class_mk64f)
target_compile_definitions(kickos_k64dspi PRIVATE
  kos_spi_bus_open=k64dspi_bus_open
  kos_spi_device_open=k64dspi_device_open
  kos_spi_transfer=k64dspi_transfer
  kos_spi_bus_close=k64dspi_bus_close)
