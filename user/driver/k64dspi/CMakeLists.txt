# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_k64dspi: the K64F/DSPI0 unprivileged SPI driver (privileged bring-up + an
# unprivileged driver thread; clients call spi_transfer/spi_enable_cs/spi_disable_cs
# -- see <kickos/driver/k64dspi.h>). Exported so an out-of-tree consumer (e.g. a
# KickCAT LAN9252 slave) links it on top of the OS. Its .data/.bss (the bounce
# buffer + descriptor) land in .appdata: libkickos_k64dspi.a is not in the mk64f.ld
# closed kernel set, so the .appdata catch-all captures it -- user-reachable.
add_library(kickos_k64dspi STATIC k64dspi.cc)
kickos_apply_freestanding(kickos_k64dspi)
# Unprivileged userspace code: kickos_user carries user/include (kos.h/sys.h) plus
# the transitive lib/arch includes, and orders the component group after this lib
# at the final link.
target_link_libraries(kickos_k64dspi PUBLIC kickos_user)
# Rule 6 (M4, decision R-A): the DSPI RX-FIFO fill-level read is shared register
# logic, so it comes from the freestanding kickos_class_mk64f leaf -- the same
# object the kernel timebase will link in Phase A -- not a local copy.
target_link_libraries(kickos_k64dspi PRIVATE kickos_class_mk64f)
install(TARGETS kickos_k64dspi EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
