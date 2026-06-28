# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(KICKOS_ARCH STREQUAL "sim")
  add_library(kickos_arch_sim STATIC sim/sim.cc sim/start.cc)
  kickos_apply_hosted(kickos_arch_sim)
  target_include_directories(kickos_arch_sim PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sim/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_sim EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  # Kernel headers for the reverse-direction callbacks are pulled via arch.h.
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sim/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "armv7m")
  # M1: context switch (PendSV) + syscall trap (SVC trampoline) in switch.S,
  # the Cortex-M core-generic seam in arch_armv7m.cc. The chip layer
  # (arch/arm/chip/*, M1 item 10) adds arch_init/arch_console_write + the
  # linker script; until then this builds as a static library only.
  add_library(kickos_arch_armv7m STATIC
    arm/armv7m/arch_armv7m.cc
    arm/armv7m/switch.S)
  kickos_apply_freestanding(kickos_arch_armv7m)
  target_include_directories(kickos_arch_armv7m PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/arm/armv7m/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_armv7m EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arm/armv7m/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "armv6m")
  # M1 item 11: Cortex-M0/M0+ (RP2040). PRIMASK crit + v6-M-subset switch.S; the
  # chip supplies arch_clock_now (no DWT) + arch_init/console + linker script.
  add_library(kickos_arch_armv6m STATIC
    arm/armv6m/arch_armv6m.cc
    arm/armv6m/switch.S)
  kickos_apply_freestanding(kickos_arch_armv6m)
  target_include_directories(kickos_arch_armv6m PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/arm/armv6m/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_armv6m EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arm/armv6m/include" PARENT_SCOPE)
else()
  message(FATAL_ERROR "KickOS: arch backend '${KICKOS_ARCH}' not available")
endif()

# --- Chip backend (startup + linker script + clocks + console) --------------
# Built when the selected board resolves to a chip that is brought up. Sources:
# every .cc + .S under arm/chip/<chip>/. Exports the linker script path to the
# parent scope so the `kickos` interface target can bake it into the app link.
if(KICKOS_CHIP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/arm/chip/${KICKOS_CHIP}")
  # CONFIGURE_DEPENDS: adding/removing a chip source reconfigures automatically.
  file(GLOB _chip_src CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/arm/chip/${KICKOS_CHIP}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/arm/chip/${KICKOS_CHIP}/*.S")
  add_library(kickos_chip_${KICKOS_CHIP} STATIC ${_chip_src})
  kickos_apply_freestanding(kickos_chip_${KICKOS_CHIP})
  target_include_directories(kickos_chip_${KICKOS_CHIP} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/arm/${KICKOS_ARCH}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include")
  install(TARGETS kickos_chip_${KICKOS_CHIP} EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  # Ship the linker script so an out-of-tree consumer can link an image (the
  # `kickos` INTERFACE bakes an INSTALL_INTERFACE -T pointing at this copy).
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/arm/chip/${KICKOS_CHIP}/${KICKOS_CHIP}.ld"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos")
  set(KICKOS_LINKER_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/arm/chip/${KICKOS_CHIP}/${KICKOS_CHIP}.ld"
    PARENT_SCOPE)
endif()
