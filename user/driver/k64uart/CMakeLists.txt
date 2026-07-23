# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_k64uart: the K64F/UART0 userspace polled UART TX console driver (an
# unprivileged driver thread + a privileged one-shot handover bring-up; see
# <kickos/driver/k64uart.h>). Exported so an out-of-tree consumer links it on top
# of the OS. It carries no shared state, so it has no .data/.bss to home in
# .appdata; the driver's recv buffer lives on its granted stack.
add_library(kickos_k64uart STATIC k64uart.cc)
kickos_apply_freestanding(kickos_k64uart)
# Unprivileged userspace code: kickos_user carries user/include (kos.h/sys.h) plus
# the transitive lib/arch includes, and orders the component group after this lib
# at the final link.
target_link_libraries(kickos_k64uart PUBLIC kickos_user)
# sys.h/abi.h pull errno.h from kickos_system (kickos_user carries it only PRIVATE).
target_include_directories(kickos_k64uart PRIVATE
  "${PROJECT_SOURCE_DIR}/system/include")
# Rule 6 (M4, decision R-A): the per-byte transmit-ready poll is the shared UART
# register logic, so it comes from the freestanding kickos_class_mk64f leaf (the
# same object the kernel console could link in Phase A), not a local copy.
target_link_libraries(kickos_k64uart PRIVATE kickos_class_mk64f)
install(TARGETS kickos_k64uart EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
