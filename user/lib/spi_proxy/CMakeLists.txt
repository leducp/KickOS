# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_spi_proxy: the PROXY backend of the SPI class <kickos/driver/spi.h>, marshalling the
# four class calls onto a bus-service endpoint (<kickos/sys/bus.h>). ONE library for every
# chip, because it speaks the chip-agnostic wire protocol: no chip register, no MMIO, so it
# lives outside the per-chip driver gate and is exported for out-of-tree consumers. It carries
# no shared state (each call frames a request on the caller's stack), so no .appdata home is
# needed.
#
# NOT in any board's service-list closure and NOT in the kickos link group: it defines the same
# four symbols a local engine does, so which one an executable gets is that executable's own
# SPI_BACKEND (cmake/kickos.cmake).
add_library(kickos_spi_proxy STATIC spi_proxy.cc)
kickos_apply_freestanding(kickos_spi_proxy)
target_link_libraries(kickos_spi_proxy PUBLIC kickos_user)
# sys.h/abi.h pull errno.h from kickos_system (kickos_user carries it only PRIVATE).
target_include_directories(kickos_spi_proxy PRIVATE
  "${PROJECT_SOURCE_DIR}/system/include")
install(TARGETS kickos_spi_proxy EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
