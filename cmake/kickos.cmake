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
# Board -> {arch, chip} resolution.
#
# The single source of truth for a board's arch/chip/-mcpu is one descriptor,
# boards/<board>/board.cmake (also included pre-project by the ARM toolchain file
# for the CPU baseline). Here we include it to read arch + chip. The chip is the
# arch/arm/chip/<chip> backend (startup, linker script, clocks, console); the sim
# has none (KICKOS_CHIP == "").
#
# Captured at include time (a called function sees the caller's list dir, not this
# file's): the in-tree boards/ dir. It is <repo>/boards in a source tree; an
# installed package has no boards/ tree, so the fallback below applies there.
# ---------------------------------------------------------------------------
get_filename_component(KICKOS_BOARDS_DIR "${CMAKE_CURRENT_LIST_DIR}/../boards" ABSOLUTE)

# The descriptor also carries KICKOS_ARCH_FAMILY (arm|rx|sim|...): the source-tree
# family that routes arch/<family>/... and the family-specific cross toolchain. A
# board that omits it falls back to a derivation from the arch (armv6m/armv7m -> arm,
# sim -> sim) so the ARM descriptors need no churn; a non-ARM family (rx) sets it.
function(kickos_derive_arch_family arch out_family)
  if(arch MATCHES "^armv")
    set(${out_family} "arm" PARENT_SCOPE)
  elseif(arch STREQUAL "sim")
    set(${out_family} "sim" PARENT_SCOPE)
  else()
    set(${out_family} "${arch}" PARENT_SCOPE)
  endif()
endfunction()

function(kickos_load_board_descriptor board out_arch out_chip out_family)
  set(_desc "${KICKOS_BOARDS_DIR}/${board}/board.cmake")
  if(EXISTS "${_desc}")
    include("${_desc}")
    set(${out_arch} "${KICKOS_ARCH}" PARENT_SCOPE)
    set(${out_chip} "${KICKOS_CHIP}" PARENT_SCOPE)
  elseif(NOT EXISTS "${KICKOS_BOARDS_DIR}"
         AND DEFINED KICKOS_ARCH AND board STREQUAL "${KICKOS_BOARD}")
    # Installed package: no boards/ tree at all, so fall back to the arch/chip this
    # package recorded (KickOSConfig) for the single board it was built for. Gated
    # on the boards/ tree being ABSENT so an in-tree unknown board still errors
    # (in-tree the host toolchain also defines KICKOS_ARCH, which alone would let a
    # typo'd board slip through as the recorded arch).
    set(${out_arch} "${KICKOS_ARCH}" PARENT_SCOPE)
    set(${out_chip} "${KICKOS_CHIP}" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "KickOS: unknown board '${board}' "
      "(no ${_desc}, and it is not the board this package was built for)")
  endif()
  # Family: honour an explicit descriptor value, else derive from the arch.
  if(KICKOS_ARCH_FAMILY)
    set(${out_family} "${KICKOS_ARCH_FAMILY}" PARENT_SCOPE)
  else()
    kickos_derive_arch_family("${KICKOS_ARCH}" _fam)
    set(${out_family} "${_fam}" PARENT_SCOPE)
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
  # KICKOS_ARCH is the single source of truth for "is this the sim" (set by the
  # toolchain + board descriptor, and recorded in the installed package -- so this
  # also holds for an out-of-tree consumer).
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

  # ESP32 (Xtensa) only: the raw objcopy .bin is NOT bootable -- the ESP32 ROM
  # loader needs the Espressif image format (magic 0xE9, segment table, checksum),
  # and a raw .bin also spans the IRAM/DRAM VMA gap. esptool.py elf2image builds
  # the bootable image from the ELF. Graceful: if esptool is not on PATH / in the
  # esp-idf env, skip the step and tell the user how to produce the image, rather
  # than failing the build (the ELF + raw .bin/.hex are still emitted).
  if(KICKOS_CHIP STREQUAL "esp32")
    find_program(KICKOS_ESPTOOL NAMES esptool.py esptool)
    if(KICKOS_ESPTOOL)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${KICKOS_ESPTOOL} --chip esp32 elf2image
                --output $<TARGET_FILE_DIR:${target}>/${target}.app.bin
                $<TARGET_FILE:${target}>
        BYPRODUCTS ${target}.app.bin
        COMMENT "esptool elf2image -> ${target}.app.bin (bootable ESP32 image)"
        VERBATIM)
    else()
      message(STATUS "KickOS: esptool not found -- ${target}.app.bin (bootable "
        "ESP32 image) not produced. Run: esptool.py --chip esp32 elf2image "
        "--output ${target}.app.bin <elf>  (the raw ${target}.bin is NOT bootable).")
    endif()
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
  kickos_load_board_descriptor("${APP_BOARD}" _arch _chip _family)

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
