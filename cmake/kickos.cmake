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

  # Espressif chips (Xtensa esp32, RISC-V esp32c6): the raw objcopy .bin is NOT
  # bootable -- the ROM loader needs the Espressif image format (magic 0xE9, segment
  # table, checksum); esptool elf2image builds it from the ELF (entry -> _start,
  # segments -> SRAM). Graceful: if esptool is not on PATH (i.e. the esp-idf env is
  # not active), skip and tell the user, rather than failing the build (the ELF +
  # raw .bin/.hex are still emitted). --chip is the KickOS chip name (esptool accepts
  # esp32 / esp32c6 verbatim). Prefer `esptool` (esptool.py is deprecated in v5).
  if(KICKOS_CHIP STREQUAL "esp32" OR KICKOS_CHIP STREQUAL "esp32c6")
    find_program(KICKOS_ESPTOOL NAMES esptool esptool.py)
    # Our app IS the image at the ROM bootloader offset (0x1000 on esp32), so the
    # first-stage ROM loads it using the header's flash mode BEFORE any code
    # reconfigures the SPI pins. esptool's elf2image default is QIO, which the esp32
    # ROM reads unreliably from that position -- it loads segment 0 then reads a
    # garbage segment-1 header (`load:0xffffffff,len:-1`) and RTC-WDT reset-loops.
    # Force DIO for esp32 (same reason esp-idf always flashes its 2nd-stage
    # bootloader as DIO). Verified on ESP32-D0WD-V3 silicon 2026-07-08.
    set(_kos_img_mode "")
    if(KICKOS_CHIP STREQUAL "esp32")
      set(_kos_img_mode --flash_mode dio)
    elseif(KICKOS_CHIP STREQUAL "esp32c6")
      # ESP32-C6: our app is a RAM-only image at flash 0x0 with NO 2nd-stage
      # bootloader, so the RISC-V ROM loader needs --ram-only-header (which implies
      # --dont-append-digest) to boot it -- a plain elf2image image is loaded but
      # never entered (`ets_loader.c 67`). DIO for the same reason as esp32 (the ROM
      # mis-reads a QIO header from the boot position -> "Checksum failure" reset
      # loop). Verified on ESP32-C6-WROOM silicon 2026-07-08.
      set(_kos_img_mode --ram-only-header --dont-append-digest --flash_mode dio)
    endif()
    if(KICKOS_ESPTOOL)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${KICKOS_ESPTOOL} --chip ${KICKOS_CHIP} elf2image ${_kos_img_mode}
                --output $<TARGET_FILE_DIR:${target}>/${target}.app.bin
                $<TARGET_FILE:${target}>
        BYPRODUCTS ${target}.app.bin
        COMMENT "esptool elf2image -> ${target}.app.bin (bootable ${KICKOS_CHIP} image)"
        VERBATIM)
    else()
      message(STATUS "KickOS: esptool not found -- ${target}.app.bin (bootable "
        "${KICKOS_CHIP} image) not produced. Activate the esp-idf env, or run: "
        "esptool --chip ${KICKOS_CHIP} elf2image --output ${target}.app.bin <elf>  "
        "(the raw ${target}.bin is NOT bootable).")
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_application(<name> SOURCES <src...> BOARD <board> [FULL_CXX])
#   Links the app against the KickOS component libraries and emits the image.
#   For sim this is a runnable host ELF whose entry (host main) lives in the
#   sim arch backend; the app must define kickos_app_main().
#
#   FULL_CXX (opt-in, docs/design-kickcat-k64f.md "Libc strategy"): compile this
#   app's C++ TUs with -fexceptions/-frtti (NOT the freestanding clamp) and link
#   the toolchain's libstdc++/libsupc++ over newlib, so exceptions + STL + RTTI
#   work. Off by default: every other app stays freestanding, no libstdc++,
#   zero-overhead. No effect on the sim (already hosted against host libstdc++).
# ---------------------------------------------------------------------------
function(kickos_add_application name)
  cmake_parse_arguments(APP "FULL_CXX" "BOARD" "SOURCES" ${ARGN})
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
  # Full-C++ opt-in: the `kickos` interface target reads this property (per
  # consuming target) to swap the freestanding clamp for -fexceptions/-frtti and
  # to keep libstdc++ in the link (drop -nostdlib++). Off -> freestanding default.
  if(APP_FULL_CXX)
    set_target_properties(${name} PROPERTIES KICKOS_FULL_CXX ON)
  endif()
  # The chip linker script is passed as a driver -T option (see the `kickos` target),
  # which CMake does NOT treat as a link dependency -- so an edited .ld would silently
  # not relink and a stale image would flash. Make it an explicit link dependency.
  if(KICKOS_LINKER_SCRIPT AND NOT (KICKOS_ARCH STREQUAL "sim"))
    set_target_properties(${name} PROPERTIES LINK_DEPENDS "${KICKOS_LINKER_SCRIPT}")
  endif()
  target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS})
  # RISC-V PMP enforcement: keep the app's own globals out of the gp-relative
  # small-data window (unreachable by an unprivileged thread) so they land in the
  # .appdata NAPOT region -- see user/CMakeLists.txt for the rationale.
  if(_arch STREQUAL "rv32imac" AND DEFINED KICKOS_HAVE_MPU AND KICKOS_HAVE_MPU)
    target_compile_options(${name} PRIVATE -msmall-data-limit=0)
  endif()
  # The OS-agnostic entry glue (-Dmain / -include app.h) rides the `kickos`
  # usage target below, so the plain add_executable path gets it too.
  target_link_libraries(${name} PRIVATE kickos)
  # MCU: emit flashable .bin/.hex (no-op on the sim).
  kickos_emit_image(${name})
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_diagnostic_app(<name> SOURCES <src...> BOARD <board>)
#   A DIAGNOSTIC (test/bring-up) app -- built ONLY when KICKOS_ENABLE_SELFTEST is
#   on, because it depends on the test-only syscall surface (kos_irq_inject,
#   kos_guard_addr, ...) that is deliberately kept OUT of the production ABI, and/or
#   deliberately faults. A production build carries no diagnostic image. Callers
#   should `if(NOT TARGET <name>) return() endif()` before registering its tests so
#   a non-diagnostic build skips them cleanly. Distinct from kickos_add_application,
#   which is for user/demo apps that build on every configuration.
# ---------------------------------------------------------------------------
function(kickos_add_diagnostic_app name)
  if(NOT KICKOS_ENABLE_SELFTEST)
    return()
  endif()
  kickos_add_application(${name} ${ARGN})
endfunction()
