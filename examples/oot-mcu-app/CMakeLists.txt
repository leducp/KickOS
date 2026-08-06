# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A standalone downstream project consuming an installed BARE-METAL KickOS
# package. It does NOT live in the KickOS tree.
#
# The MCU path needs no more ceremony than the host one: the same plain
# add_executable + target_link_libraries, and the exported `kickos` target supplies
# the whole bare-metal link recipe (component archive group, entry glue,
# -nostartfiles, the chip linker script plus a real build dependency on it).
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
# The one bare-metal extra: flashing needs a .bin/.hex, and a POST_BUILD action
# cannot ride a usage requirement, so it is an explicit opt-in line.
kickos_emit_image(oot_mcu_app)

# --- The optional wrapper, over the same source ----------------------------
# Built only to keep the shipped sugar covered out of tree; the target above is
# the reference shape.
kickos_add_application(oot_mcu_app_sugar SOURCES main.cc)

# What the KickOS build this links against resolved for those knobs, forwarded so main.cc
# can static_assert that the package's headers state the same. The CI gate passes them; a
# hand build of this example leaves them unset and asserts nothing, which is the point of
# a downstream project not having to know them. Both targets compile the same source, so
# both carry them or the images stop being comparable.
foreach(_expect KICKOS_EXPECT_MAX_THREADS KICKOS_EXPECT_USER_STACK_SIZE)
  if(DEFINED ${_expect})
    target_compile_definitions(oot_mcu_app PRIVATE ${_expect}=${${_expect}})
    target_compile_definitions(oot_mcu_app_sugar PRIVATE ${_expect}=${${_expect}})
  endif()
endforeach()
