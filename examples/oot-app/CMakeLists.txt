# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A standalone downstream project consuming the installed KickOS package -- not
# part of the KickOS tree. Plain modern CMake: add_executable + kickos link.

cmake_minimum_required(VERSION 3.24)
project(kickos_oot_app LANGUAGES C CXX)

find_package(KickOS REQUIRED)

add_executable(oot_app main.cc)
target_link_libraries(oot_app PRIVATE kickos)
