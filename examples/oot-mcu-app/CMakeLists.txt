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

# The one supported path: plain modern CMake. kickos_add_app_target is add_executable on every
# arch but x86_64, where what firmware loads is a PE32+ UEFI application CMake cannot drive a
# linker for, so the app is an OBJECT library and kickos_emit_image writes the image from its
# objects. A consumer who targets one arch may write that arch's line directly.
if(KICKOS_ARCH STREQUAL "x86_64")
  kickos_add_app_target(oot_mcu_app main.cc)
else()
  add_executable(oot_mcu_app main.cc)
endif()
target_link_libraries(oot_mcu_app PRIVATE kickos)
# The one bare-metal extra: flashing needs a .bin/.hex (an .efi where the image IS the
# deliverable), and a POST_BUILD action cannot ride a usage requirement, so it is an explicit
# opt-in line.
kickos_emit_image(oot_mcu_app)

# What the KickOS build this links against resolved for those knobs, forwarded so main.cc
# can static_assert that the package's headers state the same. The CI gate passes them; a
# hand build of this example leaves them unset and asserts nothing, which is the point of
# a downstream project not having to know them.
foreach(_expect KICKOS_EXPECT_MAX_THREADS KICKOS_EXPECT_USER_STACK_SIZE)
  if(DEFINED ${_expect})
    target_compile_definitions(oot_mcu_app PRIVATE ${_expect}=${${_expect}})
  endif()
endforeach()
