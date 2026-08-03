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

# The universal-default service-list provider: defines kickos_board_services with
# count = 0, so the default init brings no services up. Selected by KICKOS_SERVICE_LIST
# on every board that ships no service list. Its own TU so a board that DOES select a
# list never also drags this in (one definition of kickos_board_services per image).
# All the board providers (this + the pinmaps + the per-board service lists below) go
# through kickos_add_board_provider, which self-installs to KickOSTargets, so there
# is no separate hand-maintained install(TARGETS ...) list to drift out of sync.
kickos_add_board_provider(services_none SOURCE init/common/services_none.cc)

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
  kickos_add_board_provider(services_frdmk64f SOURCE init/frdmk64f/service_list.cc
    LINK kickos_k64dspi kickos_k64uart)
  # IRQ-console alternative: the same KOS_SVC_CONSOLE entry served by the two-thread
  # buffered driver, with the SPI one left out, so it links kickos_k64uartirq ALONE.
  # Never the board default: the IRQ-driven console has no silicon witness yet.
  kickos_add_board_provider(services_frdmk64f_uartirq
    SOURCE init/frdmk64f/service_list_uartirq.cc
    LINK kickos_k64uartirq)
  target_include_directories(kickos_services_frdmk64f_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "xmc4800")
  kickos_add_board_provider(services_xmc4800relax SOURCE init/xmc4800-relax/service_list.cc
    LINK kickos_xmcuart kickos_xmcssc)
  # Console-only alternative: the same KOS_SVC_CONSOLE entry with the SPI one left out,
  # so it links kickos_xmcuart ALONE. That is the point of it: the SSC driver's
  # bring-up needs a privileged root, so an image for an unprivileged root must not
  # link it at all rather than link it and not call it.
  kickos_add_board_provider(services_xmc4800relax_console
    SOURCE init/xmc4800-relax/service_list_console.cc
    LINK kickos_xmcuart)
  # IRQ-console alternative: the same KOS_SVC_CONSOLE entry served by the two-thread
  # buffered driver, so it links kickos_xmcuartirq ALONE (no polled UART, no SSC).
  # Never the board default: the IRQ-driven console has no silicon witness yet.
  kickos_add_board_provider(services_xmc4800relax_uartirq
    SOURCE init/xmc4800-relax/service_list_uartirq.cc
    LINK kickos_xmcuartirq)
  target_include_directories(kickos_services_xmc4800relax_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "esp32c6")
  # IRQ-console alternative and the board's ONLY service list, so the default stays
  # kickos_services_none: the IRQ-driven console has no silicon witness yet. kickos_c6uart
  # exports no header, so the provider declares the bring-up itself.
  kickos_add_board_provider(services_esp32c6_uartirq
    SOURCE init/esp32c6-wroom/service_list_uartirq.cc
    LINK kickos_c6uart)
  target_include_directories(kickos_services_esp32c6_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "rx72m")
  # IRQ-console alternative and the board's ONLY service list, so the default stays
  # kickos_services_none: the IRQ-driven console has no silicon witness yet. rxsci.h comes
  # from kickos_rxsci's own PUBLIC dir, hence the <rxsci.h> spelling in the provider.
  kickos_add_board_provider(services_rx72m_uartirq
    SOURCE init/rx72m/service_list_uartirq.cc
    LINK kickos_rxsci)
  target_include_directories(kickos_services_rx72m_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "esp32")
  # IRQ-console alternative and the board's ONLY service list, so the default stays
  # kickos_services_none: the IRQ-driven console has no silicon witness yet. kickos_lx6uart
  # exports no header, so the provider declares the bring-up itself.
  kickos_add_board_provider(services_esp32_uartirq
    SOURCE init/esp32-wroom/service_list_uartirq.cc
    LINK kickos_lx6uart)
  target_include_directories(kickos_services_esp32_uartirq PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "rp2350")
  # USB-CDC-console alternative and the board's ONLY service list, so the default stays
  # kickos_services_none: no part of the USB driver has a silicon witness. It also blinds
  # the pin UART the kernel console uses, which is a second reason it must never be a
  # board default (docs/design-m4.6.2-usb-cdc.md section 6.2).
  kickos_add_board_provider(services_pizero2350_usbcdc
    SOURCE init/pizero2350/service_list_usbcdc.cc
    LINK kickos_rpusb)
  target_include_directories(kickos_services_pizero2350_usbcdc PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
elseif(KICKOS_CHIP STREQUAL "rp2040")
  # Same driver, same posture, one board down. See the rp2350 branch above.
  kickos_add_board_provider(services_picopi_usbcdc
    SOURCE init/picopi/service_list_usbcdc.cc
    LINK kickos_rpusb)
  target_include_directories(kickos_services_picopi_usbcdc PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
endif()
# The sim's publishing posture, so the console-handover route the silicon boards ship
# is reachable in CI (see init/sim/service_list.cc). Its "driver" is a host write(2),
# hence sim-only. NOT the sim default: selected explicitly with
# -DKICKOS_SERVICE_LIST=kickos_services_sim, so the ordinary sim gate keeps covering
# the pre-publish route. Unlike the pure-data providers above it calls kos_* + the
# shared driver bring-up tail, so it needs user/include.
if(KICKOS_ARCH STREQUAL "sim")
  kickos_add_board_provider(services_sim SOURCE init/sim/service_list.cc)
  target_include_directories(kickos_services_sim PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
  # Make the console driver DIE after N served messages, so driver-death reclaim has a
  # witness that needs no board (tests/check_sim_drvdeath.sh). 0 = serve forever, which
  # is every other posture. A knob on the ONE sim console driver rather than a second
  # provider: a divergent copy would stop being the reference the silicon services mirror.
  if(KICKOS_SIMCON_EXIT_AFTER)
    target_compile_definitions(kickos_services_sim PRIVATE
      KICKOS_SIMCON_EXIT_AFTER=${KICKOS_SIMCON_EXIT_AFTER})
  endif()
  # The other death shape: the driver dies BEFORE it ever receives, so the handover probe
  # is what has to notice. Same gate, second case.
  if(KICKOS_SIMCON_DIE_AT_BRINGUP)
    target_compile_definitions(kickos_services_sim PRIVATE
      KICKOS_SIMCON_DIE_AT_BRINGUP=1)
  endif()
  # The two-thread shape: a SECOND driver thread holds the console's register window and
  # parks in kos_irq_wait, so the endpoint's last receiver dying no longer means the
  # device is free. The gate app carries the same knob (it holds the extra assertions and
  # the cancel); per-target is safe here because the guard never reaches a shared header.
  if(KICKOS_SIMCON_WINDOW_THREAD)
    target_compile_definitions(kickos_services_sim PRIVATE
      KICKOS_SIMCON_WINDOW_THREAD=1)
  endif()
  # The READY-TIMEOUT shape: the IRQ thread takes the register window and never sets the
  # ready flag, so root's bounded wait expires. Selects a bring-up ordered like the silicon
  # drivers (publish, claim, IRQ thread, wait, service thread), which is what makes the
  # timeout diagnostic reportable at all. No app knob: init fails, so no app runs.
  if(KICKOS_SIMCON_IRQ_WEDGE)
    target_compile_definitions(kickos_services_sim PRIVATE
      KICKOS_SIMCON_IRQ_WEDGE=1)
  endif()

  # The buffered-UART substrate made runnable with no board: a two-thread driver over a
  # loopback "device" (init/sim/service_list_uart.cc). Its own provider rather than a second
  # entry in the list above, because one service list links per image and the existing sim
  # gates must keep testing the console posture unchanged.
  kickos_add_board_provider(services_simuart SOURCE init/sim/service_list_uart.cc)
  target_include_directories(kickos_services_simuart PRIVATE
    "${PROJECT_SOURCE_DIR}/user/include")
endif()

# The header home + the init framework lib are not board providers, so they keep an
# explicit install here (kickos_system is INTERFACE: no archive, exported for headers).
install(TARGETS kickos_system kickos_default_init
        EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
