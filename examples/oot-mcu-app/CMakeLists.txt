# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A standalone downstream project consuming an installed BARE-METAL KickOS
# package. It does NOT live in the KickOS tree.
#
# The point of this example is that the MCU path needs no more ceremony than the
# host one: the same plain add_executable + target_link_libraries, and the
# exported `kickos` target supplies the whole bare-metal link recipe -- the
# component archive group, the OS-agnostic entry glue, -nostartfiles, the chip
# linker script AND a real build-system dependency on that script, so editing it
# relinks instead of leaving a stale image to be flashed.
#
# Configure with the toolchain file the package ships:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<prefix>/lib/cmake/KickOS/toolchain-arm-none-eabi.cmake \
#     -DCMAKE_PREFIX_PATH=<prefix>

cmake_minimum_required(VERSION 3.24)
project(kickos_oot_mcu_app LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

# --- The supported path: plain modern CMake -------------------------------
add_executable(oot_mcu_app main.cc)
target_link_libraries(oot_mcu_app PRIVATE kickos)
# The one bare-metal extra. Linking gives an ELF; flashing needs a .bin/.hex, and
# a POST_BUILD action cannot ride a usage requirement -- so it is one explicit
# opt-in line rather than something the link silently does for you.
kickos_emit_image(oot_mcu_app)

# --- The optional wrapper, over the same source ----------------------------
# Built here only so the sugar stays covered out of tree: it is still shipped, so
# it must keep producing an equivalent image, and it must not quietly acquire
# powers the plain path lacks. Downstream projects should not feel obliged to use
# it -- the target above is the reference shape.
kickos_add_application(oot_mcu_app_sugar SOURCES main.cc)
