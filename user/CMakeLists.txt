# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# uart_service_dev.cc is a TU of its own so the archive pulls it only for a driver that also
# links a UART class backend; merged into uart_service.cc it would make the ring side
# undefined on a board that has no backend at all.
set(KICKOS_USER_SOURCES src/syscall_stubs.cc src/init_args.cc src/console_ring.cc
                        src/driver_service.cc src/uart_service.cc src/uart_service_dev.cc
                        src/usb_cdc_service.cc)

# The newlib porting layer clashes with host libc on the sim; it is linked from
# M1 when userspace rides the ARM toolchain libc. newlib_sbrk.cc is a separate TU so
# the archive pulls it only for an app that allocates; merging it into
# newlib_stubs.cc disables the heapless-board link error (-Wl,-u,_exit force-links
# that object everywhere).
# sim_exit.cc is a TU of its own so the archive pulls it only for an app that calls exit().
if(KICKOS_ARCH STREQUAL "sim")
  list(APPEND KICKOS_USER_SOURCES src/sim_exit.cc)
else()
  list(APPEND KICKOS_USER_SOURCES src/newlib_stubs.cc src/newlib_sbrk.cc)
  # The per-thread struct _reent array and the word the kernel swaps.
  list(APPEND KICKOS_USER_SOURCES src/newlib_reent.cc)
endif()

add_library(kickos_user STATIC ${KICKOS_USER_SOURCES})
# kickos_apply_freestanding also applies -msmall-data-limit=0 under RISC-V PMP so
# this KickOS-owned lib emits no gp small-data (kernel-side capture); the gp window
# is left to the app + C++ runtime, anchored inside the .appdata grant.
kickos_apply_freestanding(kickos_user)
target_include_directories(kickos_user PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# abi.h includes <kickos/sys/errno.h>, which now lives in kickos_system. kernel/include is
# here for config/system.h alone: the reent array is indexed by thread slot, and
# KICKOS_THREAD_SLOTS is stated there. kickos_kernel takes user/include the same way.
target_include_directories(kickos_user PRIVATE
  "${PROJECT_SOURCE_DIR}/system/include"
  "${PROJECT_SOURCE_DIR}/kernel/include")
target_link_libraries(kickos_user PUBLIC kickos_arch_${KICKOS_ARCH} kickos_lib)
install(TARGETS kickos_user EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# Chip-neutral userspace libraries apps link (kickos_spi_proxy, ...). The chip
# drivers themselves now live under system/driver as board support.
add_subdirectory(lib)
