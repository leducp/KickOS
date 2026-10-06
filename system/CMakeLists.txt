# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kickos_system: home of the fleet-wide system headers (the syscall error taxonomy,
# the frozen capability-index convention, and the init-service seam) plus the composed init.

# INTERFACE library: the header home. Propagated to every app because kickos_core
# links it (root CMakeLists.txt); it contributes no archive, so it must NOT sit in
# the RESCAN link group.
add_library(kickos_system INTERFACE)
target_include_directories(kickos_system INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# Chip-specific board/device drivers (kickos_k64dspi, kickos_xmcssc, ...): exported
# libs that build unprivileged but are system board support.
add_subdirectory(driver)

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

# ---------------------------------------------------------------------------
# The SPI class backend for this image, which is the image posture's decision and not any
# app's: every backend defines the same four kos_spi_* symbols, so exactly one links.
#
# DEFAULT, the PROXY: the bus service owns the block and a client reaches it over its
# endpoint. One library for every chip, because it speaks the chip-agnostic wire protocol
# (docs/reference/bus-service.md).
#
# KICKOS_SPI_LOCAL_ENGINE=ON, the VALIDATION posture: the client owns the peripheral's window,
# and its line where the engine is interrupt-paced, and clocks the bus inline, with no service
# and no IPC. A composition naming the bus driver links the proxy as that driver's client
# whatever this selects, so an app built both ways picks its composition on this same option.
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

# kickos_system is INTERFACE: no archive, exported for its headers.
kickos_export_targets(kickos_system)
