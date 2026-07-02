# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A standalone downstream project consuming the installed KickOS package. It
# does NOT live in the KickOS tree. This one uses the plain modern-CMake path:
# a normal add_executable linked against the exported `kickos` usage target --
# no KickOS-specific macro required. (kickos_add_application() remains available
# as sugar, and on MCU targets for image emission.)

cmake_minimum_required(VERSION 3.24)
project(kickos_oot_app LANGUAGES C CXX)

find_package(KickOS REQUIRED)

add_executable(oot_app main.cc)
target_link_libraries(oot_app PRIVATE kickos)
