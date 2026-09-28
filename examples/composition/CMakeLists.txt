# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# M10 GOLDEN EXAMPLE: does not configure yet, and no gate builds it.
#
# Two roles, one file here only because the example is self-contained:
#   * the INTEGRATOR turns a composition into a system target with `kickos_compose`, which
#     admits it against the installed kernel's exported configuration and descriptions and
#     refuses a system that does not fit;
#   * the USER writes plain CMake, add_executable and target_link_libraries, linking the
#     kernel and the system target they were handed. Which system target is linked IS the
#     choice of composition; a plain app links the kernel package's KickOS::system_default.
#
# Configured exactly like examples/oot-mcu-app, against a KickOS package built for one of
# the boards systems/ names:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<prefix>/lib/cmake/KickOS/toolchain-<arch>.cmake \
#     -DCMAKE_PREFIX_PATH=<prefix>

cmake_minimum_required(VERSION 3.24)
project(kickos_composition_example LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

# --- the integrator's half ----------------------------------------------------------------
kickos_compose(example_system systems/${KICKOS_BOARD}.yaml)

# --- the user's half: plain CMake -----------------------------------------------------------
# The app and the health checker are the same source everywhere; only the sensor is the
# board's, because what it measures is.
if(KICKOS_BOARD STREQUAL "xmc4800-relax")
  set(sensor sensor_spi.cc)
elseif(KICKOS_BOARD STREQUAL "qemu-arm64")
  set(sensor sensor_rtc.cc)
elseif(KICKOS_BOARD STREQUAL "qemu-x86_64")
  set(sensor sensor_cmos.cc)
else()
  message(FATAL_ERROR "no composition for board '${KICKOS_BOARD}' in systems/")
endif()

# x86_64 firmware loads a PE32+ UEFI application, which CMake cannot link directly, so there the
# target is kickos_add_app_target's object library, as in examples/oot-mcu-app.
if(KICKOS_ARCH STREQUAL "x86_64")
  kickos_add_app_target(sensor_system app.cc health.cc ${sensor})
else()
  add_executable(sensor_system app.cc health.cc ${sensor})
endif()
target_link_libraries(sensor_system PRIVATE KickOS::kernel example_system)
kickos_emit_image(sensor_system)
