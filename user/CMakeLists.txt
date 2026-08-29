# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# uart_service_dev.cc stays its own TU: merged into uart_service.cc it would make the ring side
# undefined on a board with no UART class backend.
set(KICKOS_USER_SOURCES src/syscall_stubs.cc src/init_args.cc src/root_entry.cc
                        src/console_ring.cc src/driver_service.cc src/uart_service.cc
                        src/uart_service_dev.cc src/usb_cdc_service.cc)

# newlib_sbrk.cc stays its own TU: merging it into newlib_stubs.cc disables the heapless-board
# link error (-Wl,-u,_exit force-links that object everywhere). sim_exit.cc is separate so the
# archive pulls it only for an app that calls exit().
if(KICKOS_ARCH STREQUAL "sim")
  list(APPEND KICKOS_USER_SOURCES src/sim_exit.cc)
elseif(KICKOS_ARCH STREQUAL "x86_64")
  # exit() is the one C-library name KickOS's own userspace layer calls (uart_service.cc), so a
  # posture linking no C library has to define it.
  list(APPEND KICKOS_USER_SOURCES src/nolibc_exit.cc)
else()
  list(APPEND KICKOS_USER_SOURCES src/newlib_stubs.cc src/newlib_sbrk.cc)
  list(APPEND KICKOS_USER_SOURCES src/newlib_reent.cc)
endif()

add_library(kickos_user STATIC ${KICKOS_USER_SOURCES})
# kickos_apply_freestanding also applies -msmall-data-limit=0 under RISC-V PMP, so this
# KickOS-owned lib emits no gp small-data; the gp window is the app's plus the C++ runtime's.
kickos_apply_freestanding(kickos_user)
target_include_directories(kickos_user PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# kernel/include is here for config/system.h alone: the reent array is indexed by thread slot,
# and KICKOS_THREAD_SLOTS is stated there.
target_include_directories(kickos_user PRIVATE
  "${PROJECT_SOURCE_DIR}/system/include"
  "${PROJECT_SOURCE_DIR}/kernel/include")
target_link_libraries(kickos_user PUBLIC kickos_arch_${KICKOS_ARCH} kickos_lib)
install(TARGETS kickos_user EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

add_subdirectory(lib)
