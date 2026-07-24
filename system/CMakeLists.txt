# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kickos_system: home of the fleet-wide system headers (the syscall error taxonomy,
# the frozen capability-index convention, and the init-service seam) plus the
# default userspace init provider.

# INTERFACE library: the header home. Propagated to every app because kickos_core
# links it (root CMakeLists.txt); it contributes no archive, so it must NOT sit in
# the RESCAN link group.
add_library(kickos_system INTERFACE)
target_include_directories(kickos_system INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# Chip-specific board/device drivers (kickos_k64dspi, kickos_xmcssc, ...): exported
# libs that build unprivileged but are system board support. Added before the board
# service-list providers below, which link this build's driver targets.
add_subdirectory(driver)

# The default init provider. Three TUs: the entry (kickos_init_entry), the app-main
# run body (kickos_default_init_run), and the console bring-up step
# (kickos_console_bringup_run, which references the CMake-selected kickos_board_console
# resolved across the rescan link group). A delegating override references only the
# run body, so keeping the entry in its own TU stops it dragging in a second definition
# of kickos_init_entry.
add_library(kickos_default_init STATIC
  init/default_init_entry.cc
  init/default_init_run.cc
  init/console_bringup.cc
  init/service_list_run.cc
  init/pinmux_run.cc)
kickos_apply_freestanding(kickos_default_init)
# It must NOT link kickos_core (that would force-include app.h with -Dmain and fire
# app.h's weak-stamp block here). Give it just the include paths it needs: its own
# system/include (init.h) and user/include (app.h).
target_include_directories(kickos_default_init PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/user/include")

# The universal-default console bring-up provider: defines kickos_board_console with
# start = NULL, so the default init keeps the kernel console. Selected by
# KICKOS_CONSOLE_BRINGUP on every board that ships no userspace console driver. Its
# own TU so a board that DOES select a driver provider never also drags this in (one
# definition of kickos_board_console per image).
add_library(kickos_console_none STATIC init/console_none.cc)
kickos_apply_freestanding(kickos_console_none)
target_include_directories(kickos_console_none PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The universal-default service-list provider: defines kickos_board_services with
# count = 0, so the default init brings no services up. Selected by KICKOS_SERVICE_LIST
# on every board that ships no service list. Its own TU so a board that DOES select a
# list never also drags this in (one definition of kickos_board_services per image).
add_library(kickos_services_none STATIC init/services_none.cc)
kickos_apply_freestanding(kickos_services_none)
target_include_directories(kickos_services_none PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Board pin-map providers: each DEFINES kickos_board_pinmap (see <kickos/sys/pinmap.h>),
# selected by KICKOS_BOARD_PINMAP. Pure data TUs (no further deps); one definition of
# kickos_board_pinmap per image, so each is its own TU like kickos_console_none. pinmap.h
# needs only stdint, so system/include suffices.
add_library(kickos_pinmap_none STATIC init/pinmap_none.cc)
kickos_apply_freestanding(kickos_pinmap_none)
target_include_directories(kickos_pinmap_none PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(kickos_pinmap_xmc4800relax STATIC init/pinmap_xmc4800relax.cc)
kickos_apply_freestanding(kickos_pinmap_xmc4800relax)
target_include_directories(kickos_pinmap_xmc4800relax PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(kickos_pinmap_frdmk64f STATIC init/pinmap_frdmk64f.cc)
kickos_apply_freestanding(kickos_pinmap_frdmk64f)
target_include_directories(kickos_pinmap_frdmk64f PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Board service-list providers: each DEFINES kickos_board_services (see
# <kickos/sys/service.h>), selected by KICKOS_SERVICE_LIST. Like the pinmap providers
# above they are board descriptors, so they live in system/init; unlike them they link
# the board's driver targets, so each is gated to the chip whose drivers exist. Each
# links BOTH the console and the SPI driver so the driver + its class leaf come along.
# service.h lives in kickos_system (system/include); the driver libs it links carry
# user/include (kos.h/sys.h + the driver headers) transitively.
if(KICKOS_CHIP STREQUAL "mk64f")
  add_library(kickos_services_frdmk64f STATIC init/service_list_frdmk64f.cc)
  kickos_apply_freestanding(kickos_services_frdmk64f)
  target_link_libraries(kickos_services_frdmk64f PUBLIC kickos_k64dspi kickos_k64uart)
  target_include_directories(kickos_services_frdmk64f PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  install(TARGETS kickos_services_frdmk64f EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
elseif(KICKOS_CHIP STREQUAL "xmc4800")
  add_library(kickos_services_xmc4800relax STATIC init/service_list_xmc4800relax.cc)
  kickos_apply_freestanding(kickos_services_xmc4800relax)
  target_link_libraries(kickos_services_xmc4800relax PUBLIC kickos_xmcuart kickos_xmcssc)
  target_include_directories(kickos_services_xmc4800relax PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  install(TARGETS kickos_services_xmc4800relax EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS kickos_system kickos_default_init kickos_console_none
        kickos_services_none
        kickos_pinmap_none kickos_pinmap_xmc4800relax kickos_pinmap_frdmk64f
        EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
