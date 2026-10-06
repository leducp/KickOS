# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The composition example: an app polling a sensor service, a health checker told of each death,
# and a sensor for the board, composed by systems/<board>.yaml into the image sensor_system on
# xmc4800-relax, qemu-arm64 and qemu-x86_64.
#
# Two roles, one file here only because the example is self-contained:
#   * the INTEGRATOR turns a composition into a system target with `kickos_compose`, which
#     admits it against the installed kernel's exported configuration and descriptions and
#     refuses a system that does not fit;
#   * the USER writes plain CMake, add_executable and target_link_libraries, linking the
#     kernel and the system target they were handed. Which system target is linked IS the
#     choice of composition; a plain app links the kernel package's KickOS::system_default.
#
# Built out of tree against the installed package of a KickOS build for one of those boards, with
# the cross toolchain file the package ships, in a shell where the KickOS toolchain's
# kickos-toolchain.sh is sourced so that KICKOS_TOOLCHAIN names it:
#   cmake --install <kickos-build> --prefix <prefix>
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<prefix>/lib/cmake/KickOS/toolchain-<target>.cmake \
#     -DCMAKE_PREFIX_PATH=<prefix>
#   cmake --build build
# The image is build/sensor_system, build/sensor_system.efi on qemu-x86_64.

cmake_minimum_required(VERSION 3.24)
project(kickos_composition_example LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

# --- the integrator's half ----------------------------------------------------------------
kickos_compose(example_system systems/${KICKOS_BOARD}.yaml)

# --- the user's half: plain CMake -----------------------------------------------------------
# The app and the health checker are the same source on every board; the sensor is the board's.
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
# target is kickos_add_app_target's object library and kickos_emit_image writes the image.
if(KICKOS_ARCH STREQUAL "x86_64")
  kickos_add_app_target(sensor_system app.cc health.cc ${sensor})
else()
  add_executable(sensor_system app.cc health.cc ${sensor})
endif()
target_link_libraries(sensor_system PRIVATE KickOS::kernel example_system)
kickos_emit_image(sensor_system)
