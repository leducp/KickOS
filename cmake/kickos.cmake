# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# KickOS build helpers: per-component flag posture and the dependency-inversion
# application helper kickos_add_application().
#
# Design (architecture.md, invariant #8): the application owns the final link.
# KickOS ships object libraries + headers + startup; kickos_add_application()
# performs the link and emits the image (host ELF for sim; .bin/.hex/.uf2 for
# MCUs later). Switching sim<->MCU is a one-word BOARD change.

# ---------------------------------------------------------------------------
# Board -> arch resolution.
# ---------------------------------------------------------------------------
function(kickos_resolve_board board out_arch)
  if(board STREQUAL "sim")
    set(${out_arch} "sim" PARENT_SCOPE)
  elseif(board STREQUAL "frdmk64f")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  elseif(board STREQUAL "picopi")
    set(${out_arch} "armv6m" PARENT_SCOPE)
  elseif(board STREQUAL "f411disco")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  elseif(board STREQUAL "bluepill")
    set(${out_arch} "armv7m" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "KickOS: unknown board '${board}'")
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

set(KICKOS_FREESTANDING_FLAGS
  -ffreestanding
  -fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-use-cxa-atexit
  -fno-common)

# freestanding C++ TUs: kernel, lib, user
function(kickos_apply_freestanding target)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_compile_options(${target} PRIVATE
    ${KICKOS_WARN_FLAGS} ${KICKOS_FREESTANDING_FLAGS})
endfunction()

# hosted C++ TUs: the sim arch backend only
function(kickos_apply_hosted target)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_compile_options(${target} PRIVATE
    ${KICKOS_WARN_FLAGS} -fno-exceptions -fno-rtti)
  target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
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
  kickos_resolve_board("${APP_BOARD}" _arch)

  add_executable(${name} ${APP_SOURCES})
  target_compile_features(${name} PRIVATE cxx_std_17)
  target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS})
  # The component libraries reference each other cyclically (arch <-> kernel:
  # the arch backend calls kernel ISR/dispatch callbacks and vice versa), so
  # resolve them as one rescanned archive group.
  target_link_libraries(${name} PRIVATE
    "$<LINK_GROUP:RESCAN,kickos_user,kickos_kernel,kickos_arch_${_arch},kickos_lib>")

  if(_arch STREQUAL "sim")
    # Host ELF; nothing further to emit. Threads use POSIX in the arch backend.
    find_package(Threads REQUIRED)
    target_link_libraries(${name} PRIVATE Threads::Threads rt)
  endif()
endfunction()
