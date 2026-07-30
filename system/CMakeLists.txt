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

# The default init provider. Five TUs: the entry (kickos_init_entry), the app-main
# run body (kickos_default_init_run), the service-list walk (kickos_service_list_run,
# which references the CMake-selected kickos_board_services resolved across the rescan
# link group), the pin-mux step, and the fallback kickos_app_authority. A delegating
# override references only the run body, so keeping the entry in its own TU stops it
# dragging in a second definition of kickos_init_entry.
#
# app_authority_default.cc must stay a ONE-SYMBOL member: an app defining
# kickos_app_authority resolves it locally so this member is never extracted, which is
# what lets both definitions be strong. Give it a second symbol and any app that declares
# its own mask fails to link with a duplicate definition.
add_library(kickos_default_init STATIC
  init/default_init_entry.cc
  init/default_init_run.cc
  init/service_list_run.cc
  init/pinmux_run.cc
  init/app_authority_default.cc)
kickos_apply_freestanding(kickos_default_init)
# It must NOT link kickos_core (that would force-include app.h with -Dmain and fire
# app.h's weak-stamp block here). Give it just the include paths it needs: its own
# system/include (init.h) and user/include (app.h).
target_include_directories(kickos_default_init PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/user/include")

# The universal-default service-list provider: defines kickos_board_services with
# count = 0, so the default init brings no services up. Selected by KICKOS_SERVICE_LIST
# on every board that ships no service list. Its own TU so a board that DOES select a
# list never also drags this in (one definition of kickos_board_services per image).
# All the board providers (this + the pinmaps + the per-board service lists below) go
# through kickos_add_board_provider, which self-installs to KickOSTargets, so there
# is no separate hand-maintained install(TARGETS ...) list to drift out of sync.
kickos_add_board_provider(services_none SOURCE init/services_none.cc)

# Board pin-map providers: each DEFINES kickos_board_pinmap (see <kickos/sys/pinmap.h>),
# selected by KICKOS_BOARD_PINMAP. Pure data TUs (no further deps); one definition of
# kickos_board_pinmap per image, so each is its own TU like kickos_services_none. pinmap.h
# needs only stdint, so system/include suffices.
kickos_add_board_provider(pinmap_none SOURCE init/pinmap_none.cc)
kickos_add_board_provider(pinmap_xmc4800relax SOURCE init/pinmap_xmc4800relax.cc)
kickos_add_board_provider(pinmap_frdmk64f SOURCE init/pinmap_frdmk64f.cc)
kickos_add_board_provider(pinmap_picopi SOURCE init/pinmap_picopi.cc)
kickos_add_board_provider(pinmap_f302nucleo SOURCE init/pinmap_f302nucleo.cc)

# Board service-list providers: each DEFINES kickos_board_services (see
# <kickos/sys/service.h>), selected by KICKOS_SERVICE_LIST. Like the pinmap providers
# above they are board descriptors, so they live in system/init; unlike them they link
# the board's driver targets (LINK), so each is gated to the chip whose drivers exist.
# Each links BOTH the console and the SPI driver so the driver + its class leaf come
# along. service.h lives in kickos_system (system/include); the driver libs it links
# carry user/include (kos.h/sys.h + the driver headers) transitively.
if(KICKOS_CHIP STREQUAL "mk64f")
  kickos_add_board_provider(services_frdmk64f SOURCE init/service_list_frdmk64f.cc
    LINK kickos_k64dspi kickos_k64uart)
elseif(KICKOS_CHIP STREQUAL "xmc4800")
  kickos_add_board_provider(services_xmc4800relax SOURCE init/service_list_xmc4800relax.cc
    LINK kickos_xmcuart kickos_xmcssc)
  # Console-only alternative: the same KOS_SVC_CONSOLE entry with the SPI one left out,
  # so it links kickos_xmcuart ALONE. That is the point of it: the SSC driver's
  # bring-up needs a privileged root, so an image for an unprivileged root must not
  # link it at all rather than link it and not call it.
  kickos_add_board_provider(services_xmc4800relax_console
    SOURCE init/service_list_xmc4800relax_console.cc
    LINK kickos_xmcuart)
endif()
# The sim's publishing posture, so the console-handover route the silicon boards ship
# is reachable in CI (see init/service_list_sim.cc). Its "driver" is a host write(2),
# hence sim-only. NOT the sim default: selected explicitly with
# -DKICKOS_SERVICE_LIST=kickos_services_sim, so the ordinary sim gate keeps covering
# the pre-publish route. Unlike the pure-data providers above it calls kos_* + the
# shared driver bring-up tail, so it needs user/include.
if(KICKOS_ARCH STREQUAL "sim")
  kickos_add_board_provider(services_sim SOURCE init/service_list_sim.cc)
  target_include_directories(kickos_services_sim PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
endif()

# The header home + the init framework lib are not board providers, so they keep an
# explicit install here (kickos_system is INTERFACE: no archive, exported for headers).
install(TARGETS kickos_system kickos_default_init
        EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
