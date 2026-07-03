# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# KickOS build helpers: per-component flag posture and the dependency-inversion
# application helper kickos_add_application().
#
# Design (architecture.md, invariant #8): the application owns the final link.
# KickOS ships static libraries + headers + startup; kickos_add_application()
# performs the link and emits the image (host ELF for sim; .bin/.hex/.uf2 for
# MCUs later). Switching sim<->MCU is a one-word BOARD change.

# ---------------------------------------------------------------------------
# Board -> arch resolution.
# ---------------------------------------------------------------------------
function(kickos_resolve_board board out_arch)
  if(board STREQUAL "sim")
    set(${out_arch} "sim" PARENT_SCOPE)
  elseif(board STREQUAL "qemu")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  elseif(board STREQUAL "frdmk64f")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  elseif(board STREQUAL "picopi")
    set(${out_arch} "armv6m" PARENT_SCOPE)
  elseif(board STREQUAL "microbit")
    set(${out_arch} "armv6m" PARENT_SCOPE)
  elseif(board STREQUAL "f411disco")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  elseif(board STREQUAL "bluepill")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  elseif(board STREQUAL "f302nucleo")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  elseif(board STREQUAL "due")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  elseif(board STREQUAL "xmc4800")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "KickOS: unknown board '${board}'")
  endif()
endfunction()

# Board -> chip (the arch/arm/chip/<chip> backend: startup, linker script,
# clocks, console). The sim has no chip. Chips land per M1 step.
function(kickos_resolve_chip board out_chip)
  if(board STREQUAL "qemu")
    set(${out_chip} "mps2" PARENT_SCOPE)
  elseif(board STREQUAL "microbit")
    set(${out_chip} "nrf51" PARENT_SCOPE)
  elseif(board STREQUAL "frdmk64f")
    set(${out_chip} "mk64f" PARENT_SCOPE)
  elseif(board STREQUAL "picopi")
    set(${out_chip} "rp2040" PARENT_SCOPE)
  elseif(board STREQUAL "f411disco")
    set(${out_chip} "stm32f411" PARENT_SCOPE)
  elseif(board STREQUAL "bluepill")
    set(${out_chip} "stm32f103" PARENT_SCOPE)
  elseif(board STREQUAL "f302nucleo")
    set(${out_chip} "stm32f302" PARENT_SCOPE)
  elseif(board STREQUAL "due")
    set(${out_chip} "sam3x8e" PARENT_SCOPE)
  elseif(board STREQUAL "xmc4800")
    set(${out_chip} "xmc4800" PARENT_SCOPE)
  else()
    set(${out_chip} "" PARENT_SCOPE) # sim, or a chip not yet brought up
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Flag posture.
#   Kernel / lib / userspace  -> freestanding C++ (no exceptions/rtti).
#   arch/sim                  -> hosted (bridges to host libc), still no exc/rtti.
# Applied per-target so a hosted arch TU and a freestanding kernel TU coexist
# in one binary.
# ---------------------------------------------------------------------------
set(KICKOS_WARN_FLAGS
  -Wall -Wextra -Wshadow -Wundef)

# Flags valid for every language (C, C++, ASM); the C++-only ones are guarded
# below so a target mixing .cc and .S (the ARM arch backends) stays warning-free.
set(KICKOS_FREESTANDING_FLAGS
  -ffreestanding
  -fno-common)
set(KICKOS_FREESTANDING_CXX_FLAGS
  -fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-use-cxa-atexit)

# freestanding TUs: kernel, lib, user, and the ARM arch backends (C++ + ASM).
function(kickos_apply_freestanding target)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_compile_options(${target} PRIVATE
    ${KICKOS_WARN_FLAGS} ${KICKOS_FREESTANDING_FLAGS}
    "$<$<COMPILE_LANGUAGE:CXX>:${KICKOS_FREESTANDING_CXX_FLAGS}>")
endfunction()

# hosted C++ TUs: the sim arch backend only
function(kickos_apply_hosted target)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_compile_options(${target} PRIVATE
    ${KICKOS_WARN_FLAGS} -fno-exceptions -fno-rtti)
  target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
endfunction()

# The per-chip -mcpu/-mfpu/-mfloat-abi baseline is set globally by the ARM
# toolchain file (CMAKE_<LANG>_FLAGS_INIT) so it applies uniformly to every
# compile and link (correct multilib). Freestanding TUs on ARM therefore need
# no extra CPU flags here -- kickos_apply_freestanding() is arch-agnostic.

# ---------------------------------------------------------------------------
# kickos_emit_image(<target>)
#   MCU only: turn a linked ELF into flashable .bin and .hex, and print size.
#   No-op on the sim (a runnable host ELF is the deliverable there).
# ---------------------------------------------------------------------------
function(kickos_emit_image target)
  # Key on KICKOS_ARCH (which the installed package records) rather than
  # KICKOS_IS_SIM (a toolchain-only var an out-of-tree consumer never sees, which
  # would wrongly objcopy a host sim ELF).
  if(KICKOS_ARCH STREQUAL "sim")
    return()
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}> $<TARGET_FILE_DIR:${target}>/${target}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${target}> $<TARGET_FILE_DIR:${target}>/${target}.hex
    BYPRODUCTS ${target}.bin ${target}.hex
    VERBATIM)
  if(CMAKE_SIZE)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target}>
      VERBATIM)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_application(<name> SOURCES <src...> BOARD <board>)
#   Links the app against the KickOS component libraries and emits the image.
#   For sim this is a runnable host ELF whose entry (host main) lives in the
#   sim arch backend; the app must define kickos_app_main().
# ---------------------------------------------------------------------------
function(kickos_add_application name)
  cmake_parse_arguments(APP "" "BOARD" "SOURCES" ${ARGN})
  if(NOT APP_SOURCES)
    message(FATAL_ERROR "kickos_add_application(${name}): SOURCES required")
  endif()
  if(NOT APP_BOARD)
    set(APP_BOARD "${KICKOS_BOARD}")
  endif()
  if(NOT APP_BOARD)
    message(FATAL_ERROR "kickos_add_application(${name}): no BOARD given and the "
      "KickOS package records no default board")
  endif()
  kickos_resolve_board("${APP_BOARD}" _arch)

  # The installed package was built for one board/arch. Fail clearly rather than
  # letting a missing target degrade to a bare -lkickos_arch_<arch> link error.
  if(NOT TARGET kickos_arch_${_arch})
    message(FATAL_ERROR "kickos_add_application(${name}): BOARD '${APP_BOARD}' "
      "needs arch '${_arch}', but this KickOS package provides no "
      "kickos_arch_${_arch} (it was built for a different board)")
  endif()

  # Optional sugar over the plain path:
  #   add_executable(${name} ...) ; target_link_libraries(${name} PRIVATE kickos)
  # The `kickos` interface target carries the component link group (+ threads on
  # sim). (MCU image emission -- objcopy .bin/.uf2 -- will hang off here at M1.)
  add_executable(${name} ${APP_SOURCES})
  target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS})
  # The OS-agnostic entry glue (-Dmain / -include app.h) rides the `kickos`
  # usage target below, so the plain add_executable path gets it too.
  target_link_libraries(${name} PRIVATE kickos)
  # MCU: emit flashable .bin/.hex (no-op on the sim).
  kickos_emit_image(${name})
endfunction()
