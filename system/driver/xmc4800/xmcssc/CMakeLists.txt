# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# TWO TARGETS OVER ONE ENGINE, and the split is the driver-model ruling.
#
# kickos_spi_xmcssc is the SSC backend of the SPI class <kickos/driver/spi.h>: the public
# kos_spi_* symbols, for a consumer that owns the U0C1 window and clocks the bus inline. It is
# what a consumer target names as SPI_BACKEND (cmake/kickos.cmake). Deliberately NOT a link
# dependency of the service below, so it never enters a board's service-list closure or the
# shared kickos link group: which backend an executable gets belongs to that executable.
#
# kickos_xmcssc is the SERVICE: the bring-up shim, the endpoint, the driver thread, and its OWN
# private copy of the same engine. An image hosting this service may also hold a consumer that
# reached the public class names through kickos_spi_proxy, and one ELF has one meaning per
# symbol, so the service's four class symbols are RENAMED below. A fifth class call that
# forgets its rename here is reported as a duplicate symbol ONLY where another definer of that
# public name is in the same link; forgetting the rename on ALL FOUR is not reported at all,
# and tests/static/check_class_backend.sh is what catches that. See xmcssc.cc.
#
# REGDIR carries regs/usic.h (the shared USIC offset + bit-field header): definitions only, no
# code. No class leaf: the SSC engine needs no shared register-logic object.

kickos_add_driver(spi_xmcssc SOURCES spi_usic.cc REGDIR arch/arm/chip/xmc4800)

kickos_add_driver(xmcssc SOURCES xmcssc.cc spi_usic.cc REGDIR arch/arm/chip/xmc4800)
target_compile_definitions(kickos_xmcssc PRIVATE
  kos_spi_bus_open=xmcssc_bus_open
  kos_spi_device_open=xmcssc_device_open
  kos_spi_transfer=xmcssc_transfer
  kos_spi_bus_close=xmcssc_bus_close)
