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
  init/common/default_init_entry.cc
  init/common/default_init_run.cc
  init/common/service_list_run.cc
  init/common/pinmux_run.cc
  init/common/app_authority_default.cc)
kickos_apply_freestanding(kickos_default_init)
# It must NOT link kickos_core (that would force-include app.h with -Dmain and fire
# app.h's build-stamp block here). Give it just the include paths it needs: its own
# system/include (init.h) and user/include (app.h).
target_include_directories(kickos_default_init PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/user/include")

# kickos_root_lower for an image linking no system target: the old leaves' link group carries
# it beside the init provider, and a system target's init defines its own.
add_library(kickos_root_lower STATIC init/common/root_lower.cc)
kickos_apply_freestanding(kickos_root_lower)
target_include_directories(kickos_root_lower PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/user/include")

# A system target's init (docs/design-m10-target.md, sections 1 and 5): the walk over the emitted
# table and kickos_init_entry. Objects rather than an archive, so the link always takes them. No
# image links it here: a system target carries it as KickOS::init.
add_library(kickos_init_compose OBJECT
  init/compose/entry.cc
  init/compose/walk.cc
  init/compose/message.cc)
# The walk's driver path, KickOS::init_drivers where the composition names a packaged driver and
# KickOS::init_no_drivers where it names none.
add_library(kickos_init_drivers OBJECT init/compose/driver_path.cc)
add_library(kickos_init_no_drivers OBJECT init/compose/driver_path_none.cc)
foreach(_init kickos_init_compose kickos_init_drivers kickos_init_no_drivers)
  kickos_apply_freestanding(${_init})
  target_link_libraries(${_init} PRIVATE kickos_user)
  target_include_directories(${_init} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
endforeach()
set_target_properties(kickos_init_compose PROPERTIES EXPORT_NAME init)
set_target_properties(kickos_init_drivers PROPERTIES EXPORT_NAME init_drivers)
set_target_properties(kickos_init_no_drivers PROPERTIES EXPORT_NAME init_no_drivers)
kickos_export_targets(kickos_init_compose kickos_init_drivers kickos_init_no_drivers)

# KickOS::main: kickos_main, the entry of a composition's task that runs the app's main. A system
# target carries its object where its composition names it.
add_library(kickos_main_entry OBJECT init/compose/main.cc)
kickos_apply_freestanding(kickos_main_entry)
target_link_libraries(kickos_main_entry PRIVATE kickos_user)
target_include_directories(kickos_main_entry PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(kickos_main_entry PROPERTIES EXPORT_NAME main)
kickos_export_targets(kickos_main_entry)

# The universal-default service-list provider: defines kickos_board_services with
# count = 0, so the default init brings no services up. Selected by KICKOS_SERVICE_LIST
# on every board that ships no service list. Its own TU so a board that DOES select a
# list never also drags this in (one definition of kickos_board_services per image).
# All the board providers (this + the pinmaps + the per-board service lists below) go
# through kickos_add_board_provider, which exports each one, so there
# is no separate hand-maintained install(TARGETS ...) list to drift out of sync.
kickos_add_board_provider(services_none SOURCE init/common/services_none.cc
  RETAINED_ENDPOINTS 0)

# Board pin-map providers: each DEFINES kickos_board_pinmap (see <kickos/sys/pinmap.h>),
# selected by KICKOS_BOARD_PINMAP. Pure data TUs (no further deps); one definition of
# kickos_board_pinmap per image, so each is its own TU like kickos_services_none. pinmap.h
# needs only stdint, so system/include suffices.
kickos_add_board_provider(pinmap_none SOURCE init/common/pinmap_none.cc)
kickos_add_board_provider(pinmap_xmc4800relax SOURCE init/xmc4800-relax/pinmap.cc)
kickos_add_board_provider(pinmap_frdmk64f SOURCE init/frdmk64f/pinmap.cc)
kickos_add_board_provider(pinmap_picopi SOURCE init/picopi/pinmap.cc)
kickos_add_board_provider(pinmap_f302nucleo SOURCE init/f302nucleo/pinmap.cc)

# Board service-list providers: each DEFINES kickos_board_services (see
# <kickos/sys/service.h>), selected by KICKOS_SERVICE_LIST. Like the pinmap providers
# above they are board descriptors, so they live in system/init; unlike them they link
# the board's driver targets (LINK), so each is gated to the chip whose drivers exist.
# Each links BOTH the console and the SPI driver so the driver + its class leaf come
# along. service.h lives in kickos_system (system/include); the driver libs it links
# carry user/include (kos.h/sys.h + the driver headers) transitively.
if(KICKOS_CHIP STREQUAL "mk64f")
  # RETAINED_CAPS 1: k64dspi keeps its SPI request endpoint (g_spi0_ep) in root's table for
  # the life of the image. The console entry retains no INDEX; console_handover_finish
  # closes root's cap, and the published route lives at the reserved index 0.
  # RETAINED_ENDPOINTS 2: that reserved index still names the console endpoint, so its POOL
  # SLOT is root's for the life of the image too, and the SPI one is the second.
  kickos_add_board_provider(services_frdmk64f SOURCE init/frdmk64f/service_list.cc
    LINK kickos_k64dspi kickos_k64uart RETAINED_CAPS 1 RETAINED_ENDPOINTS 2)
  # IRQ-console alternative: the same KOS_SVC_CONSOLE entry served by the two-thread
  # buffered driver, with the SPI one left out, so it links kickos_k64uartirq ALONE.
  # Never the board default: the IRQ-driven console has no silicon witness yet.
  kickos_add_board_provider(services_frdmk64f_uartirq
    SOURCE init/frdmk64f/service_list_uartirq.cc
    LINK kickos_k64uartirq RETAINED_ENDPOINTS 1)
  target_include_directories(kickos_services_frdmk64f_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "xmc4800")
  # RETAINED_CAPS 1 and RETAINED_ENDPOINTS 2, for the reason the mk64f list above gives:
  # xmcssc keeps its SPI request endpoint (g_spi0_ep) in root's table, and the console
  # retains a pool slot at the reserved index without retaining an index of its own.
  kickos_add_board_provider(services_xmc4800relax SOURCE init/xmc4800-relax/service_list.cc
    LINK kickos_xmcuart kickos_xmcssc RETAINED_CAPS 1 RETAINED_ENDPOINTS 2)
  # Console-only alternative: the same KOS_SVC_CONSOLE entry with the SPI one left out,
  # so it links kickos_xmcuart ALONE. That is the point of it: the SSC driver's
  # bring-up needs a privileged root, so an image for an unprivileged root must not
  # link it at all rather than link it and not call it.
  kickos_add_board_provider(services_xmc4800relax_console
    SOURCE init/xmc4800-relax/service_list_console.cc
    LINK kickos_xmcuart RETAINED_ENDPOINTS 1)
  # IRQ-console alternative: the same KOS_SVC_CONSOLE entry served by the two-thread
  # buffered driver, so it links kickos_xmcuartirq ALONE (no polled UART, no SSC).
  # Never the board default: the IRQ-driven console has no silicon witness yet.
  kickos_add_board_provider(services_xmc4800relax_uartirq
    SOURCE init/xmc4800-relax/service_list_uartirq.cc
    LINK kickos_xmcuartirq RETAINED_ENDPOINTS 1)
  target_include_directories(kickos_services_xmc4800relax_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "esp32c6")
  # IRQ-console alternative and the board's ONLY service list, so the default stays
  # kickos_services_none. Witnessed on silicon 2026-08-07: 1..84 all ok. kickos_c6uart
  # exports no header, so the provider declares the bring-up itself.
  kickos_add_board_provider(services_esp32c6_uartirq
    SOURCE init/esp32c6-wroom/service_list_uartirq.cc
    LINK kickos_c6uart RETAINED_ENDPOINTS 1)
  target_include_directories(kickos_services_esp32c6_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "rx72m")
  # IRQ-console alternative and the board's ONLY service list, so the default stays
  # kickos_services_none. Witnessed on silicon 2026-08-07: 1..95 all ok. rxsci.h comes
  # from kickos_rxsci's own PUBLIC dir, hence the <rxsci.h> spelling in the provider.
  kickos_add_board_provider(services_rx72m_uartirq
    SOURCE init/rx72m/service_list_uartirq.cc
    LINK kickos_rxsci RETAINED_ENDPOINTS 1)
  target_include_directories(kickos_services_rx72m_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "esp32")
  # IRQ-console alternative and the board's ONLY service list, so the default stays
  # kickos_services_none. Witnessed on silicon 2026-08-07: 1..91 all ok. kickos_lx6uart
  # exports no header, so the provider declares the bring-up itself.
  kickos_add_board_provider(services_esp32_uartirq
    SOURCE init/esp32-wroom/service_list_uartirq.cc
    LINK kickos_lx6uart RETAINED_ENDPOINTS 1)
  target_include_directories(kickos_services_esp32_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "rp2350")
  # Never a board default: it blinds the pin UART the kernel console uses.
  kickos_add_board_provider(services_pizero2350_usbcdc
    SOURCE init/pizero2350/service_list_usbcdc.cc
    LINK kickos_rpusb RETAINED_ENDPOINTS 1)
  target_include_directories(kickos_services_pizero2350_usbcdc PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "rp2040")
  # Never a board default: it blinds the pin UART the kernel console uses.
  kickos_add_board_provider(services_picopi_usbcdc
    SOURCE init/picopi/service_list_usbcdc.cc
    LINK kickos_rpusb RETAINED_ENDPOINTS 1)
  target_include_directories(kickos_services_picopi_usbcdc PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "imxrt1062")
  kickos_add_board_provider(services_teensy41_usbcdc
    SOURCE init/teensy41/service_list_usbcdc.cc
    LINK kickos_rt1062usb RETAINED_ENDPOINTS 1)
  target_include_directories(kickos_services_teensy41_usbcdc PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "stm32f411")
  # IRQ-console alternative and the board's ONLY service list, so the default stays
  # kickos_services_none. The driver is per-chip and blackpill shares it; the window base
  # and rate are board facts, so they live in this per-board provider.
  kickos_add_board_provider(services_f411disco_uartirq
    SOURCE init/f411disco/service_list_uartirq.cc
    LINK kickos_f4uartirq RETAINED_ENDPOINTS 1)
  target_include_directories(kickos_services_f411disco_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
endif()
# Not the sim default: the ordinary sim gate must keep covering the pre-publish route.
if(KICKOS_ARCH STREQUAL "sim")
  # RETAINED_ENDPOINTS 1: the console handover. Every one of kickos_simcon's three start paths
  # publishes exactly one endpoint, the two KICKOS_SIMCON_* variants included.
  kickos_add_board_provider(services_sim SOURCE init/sim/service_list.cc
    LINK kickos_simcon RETAINED_ENDPOINTS 1)

  # The buffered-UART substrate made runnable with no board. Its own provider rather than a
  # second entry in the list above, because one service list links per image and the existing
  # sim gates must keep testing the console posture unchanged.
  # RETAINED_CAPS 1 and RETAINED_ENDPOINTS 1: g_uart_ep stays in root's table until the app
  # takes it, and the app runs on that same table, so both the index and the pool slot are
  # spoken for either way.
  kickos_add_board_provider(services_simuart SOURCE init/sim/service_list_uart.cc
    LINK kickos_simuart RETAINED_CAPS 1 RETAINED_ENDPOINTS 1)
endif()

# ---------------------------------------------------------------------------
# The SPI class backend for this image, which is the image posture's decision and not any
# app's: every backend defines the same four kos_spi_* symbols, so exactly one links.
#
# DEFAULT, the PROXY: the board service list brings the bus service up before main and a
# client reaches the block over its endpoint. One library for every chip, because it speaks
# the chip-agnostic wire protocol (docs/reference/bus-service.md).
#
# KICKOS_SPI_LOCAL_ENGINE=ON, the VALIDATION posture: the client owns the peripheral's window
# and its interrupt line and clocks the bus inline, with no service and no IPC. It needs a
# service list that does NOT start the bus service, or the driver thread would already hold
# the window and the grant would be refused (one owner per block):
#   cmake -B <dir> --preset xmc4800-relax-st -DKICKOS_SPI_LOCAL_ENGINE=ON \
#         -DKICKOS_SERVICE_LIST=kickos_services_xmc4800relax_console
# The app source must carry a local arm; where it does not, the endpoint call it keeps making
# is undefined at link time rather than silently absent at run time.
option(KICKOS_SPI_LOCAL_ENGINE
  "Answer the SPI class with the chip's LOCAL engine (the client owns the block) instead of the bus-service proxy"
  OFF)

set(_kickos_spi_local "")
if(KICKOS_CHIP STREQUAL "mk64f")
  set(_kickos_spi_local kickos_spi_k64dspi)
elseif(KICKOS_CHIP STREQUAL "xmc4800")
  set(_kickos_spi_local kickos_spi_xmcssc)
endif()

if(KICKOS_SPI_LOCAL_ENGINE)
  if(NOT _kickos_spi_local)
    message(FATAL_ERROR "KICKOS_SPI_LOCAL_ENGINE=ON, but chip '${KICKOS_CHIP}' ships no local "
      "SPI engine. Unset it to answer the SPI class with the bus-service proxy.")
  endif()
  kickos_select_class_backend(spi KickOS::${_kickos_spi_local})
elseif(TARGET kickos_spi_proxy)
  kickos_select_class_backend(spi KickOS::kickos_spi_proxy)
endif()

# The header home and the init framework lib are not board providers, so they are exported
# here (kickos_system is INTERFACE: no archive, exported for headers).
kickos_export_targets(kickos_system kickos_default_init kickos_root_lower)
