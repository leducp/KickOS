# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_xmcssc: the XMC4800/USIC0-CH1 SSC unprivileged SPI driver (privileged
# bring-up + an unprivileged driver thread; clients call spi_transfer/spi_transact/
# spi_config -- see <kickos/driver/xmcssc.h>). Exported so an out-of-tree consumer
# links it on top of the OS. Its .data/.bss (the endpoint handle) land in .appdata:
# libkickos_xmcssc.a is not in the xmc4800 closed kernel set, so the .appdata
# catch-all captures it -- user-reachable.
add_library(kickos_xmcssc STATIC xmcssc.cc)
kickos_apply_freestanding(kickos_xmcssc)
# Unprivileged userspace code: kickos_user carries user/include (kos.h/sys.h) plus
# the transitive lib/arch includes, and orders the component group after this lib
# at the final link.
target_link_libraries(kickos_xmcssc PUBLIC kickos_user)
# sys.h/abi.h pull errno.h from kickos_system (kickos_user carries it only PRIVATE).
# The chip register dir carries regs/usic.h (the shared USIC offset+bit-field header,
# same source the class leaf consumes) -- definitions only, no code, no kernel headers.
target_include_directories(kickos_xmcssc PRIVATE
  "${PROJECT_SOURCE_DIR}/system/include"
  "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
install(TARGETS kickos_xmcssc EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# The xmc4800-relax service-list provider (kickos_services_xmc4800relax) is a board
# descriptor, so it lives in system/init (service_list_xmc4800relax.cc) alongside
# pinmap_xmc4800relax and is declared in system/CMakeLists.txt; it links
# kickos_xmcuart + kickos_xmcssc there.
