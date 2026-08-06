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

# What the KickOS build this links against resolved for those knobs, forwarded so main.cc
# can static_assert that the package's headers state the same. The CI gate passes them; a
# hand build of this example leaves them unset and asserts nothing, which is the point of
# a downstream project not having to know them.
foreach(_expect KICKOS_EXPECT_MAX_THREADS KICKOS_EXPECT_USER_STACK_SIZE)
  if(DEFINED ${_expect})
    target_compile_definitions(oot_app PRIVATE ${_expect}=${${_expect}})
  endif()
endforeach()
