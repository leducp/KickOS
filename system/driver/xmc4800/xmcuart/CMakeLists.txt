# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_xmcuart: the XMC4800 userspace polled UART TX console driver (an
# unprivileged driver thread + a privileged one-shot handover bring-up; see
# <kickos/driver/xmcuart.h>). Exported so an out-of-tree consumer links it on top
# of the OS. It carries no shared state, so it has no .data/.bss to home in
# .appdata; the driver's recv buffer lives on its granted stack.
add_library(kickos_xmcuart STATIC xmcuart.cc)
kickos_apply_freestanding(kickos_xmcuart)
# Unprivileged userspace code: kickos_user carries user/include (kos.h/sys.h) plus
# the transitive lib/arch includes, and orders the component group after this lib
# at the final link.
target_link_libraries(kickos_xmcuart PUBLIC kickos_user)
# sys.h/abi.h pull errno.h from kickos_system (kickos_user carries it only PRIVATE).
# The chip register dir carries regs/usic.h (the shared USIC offset+bit-field header,
# same source the class leaf consumes) -- definitions only, no code, no kernel headers.
target_include_directories(kickos_xmcuart PRIVATE
  "${PROJECT_SOURCE_DIR}/system/include"
  "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
# Rule 6 (M4, decision R-A): the per-byte transmit-ready poll is the shared USIC
# register logic, so it comes from the freestanding kickos_class_xmc4800 leaf --
# the same object the kernel timebase will link in Phase A -- not a local copy.
target_link_libraries(kickos_xmcuart PRIVATE kickos_class_xmc4800)
install(TARGETS kickos_xmcuart EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# The xmc4800-relax service-list provider (kickos_services_xmc4800relax) now spans
# BOTH the console and the SSC (SPI) service, so it lives with the SSC driver
# (user/driver/xmcssc/service_list.cc) and links kickos_xmcuart + kickos_xmcssc --
# mirroring how the frdmk64f list lives with the DSPI driver.
