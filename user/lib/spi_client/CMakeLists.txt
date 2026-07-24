# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_spi_client: the CHIP-NEUTRAL SPI client wrapper (spi_transfer /
# spi_transact / spi_config over the bus-service call/reply ABI; see
# <kickos/driver/spi_client.h>). No chip register, no MMIO -- links against ANY SPI
# bus driver (K64F DSPI, XMC USIC, ...), so it lives outside the per-chip driver
# gate and is exported for out-of-tree consumers. It carries no shared state (each
# call frames a request on the caller's stack), so no .appdata home is needed.
add_library(kickos_spi_client STATIC spi_client.cc)
kickos_apply_freestanding(kickos_spi_client)
target_link_libraries(kickos_spi_client PUBLIC kickos_user)
# sys.h/abi.h pull errno.h from kickos_system (kickos_user carries it only PRIVATE).
target_include_directories(kickos_spi_client PRIVATE
  "${PROJECT_SOURCE_DIR}/system/include")
install(TARGETS kickos_spi_client EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
