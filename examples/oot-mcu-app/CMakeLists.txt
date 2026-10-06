# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Configure with the toolchain file the package ships:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<prefix>/lib/cmake/KickOS/toolchain-<target>.cmake \
#     -DCMAKE_PREFIX_PATH=<prefix>

cmake_minimum_required(VERSION 3.24)
project(kickos_oot_mcu_app LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

add_executable(oot_mcu_app main.cc)
target_link_libraries(oot_mcu_app PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(oot_mcu_app)

foreach(_expect KICKOS_EXPECT_MAX_THREADS KICKOS_EXPECT_USER_STACK_SIZE)
  if(DEFINED ${_expect})
    target_compile_definitions(oot_mcu_app PRIVATE ${_expect}=${${_expect}})
  endif()
endforeach()
