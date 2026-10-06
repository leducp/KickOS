# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
project(kickos_oot_app LANGUAGES C CXX)

find_package(KickOS REQUIRED)

add_executable(oot_app main.cc)
target_link_libraries(oot_app PRIVATE KickOS::kernel KickOS::system_default)

foreach(_expect KICKOS_EXPECT_MAX_THREADS KICKOS_EXPECT_USER_STACK_SIZE)
  if(DEFINED ${_expect})
    target_compile_definitions(oot_app PRIVATE ${_expect}=${${_expect}})
  endif()
endforeach()
