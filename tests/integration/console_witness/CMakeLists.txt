# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The console restart witness's system (tests/integration/check_console_restart.sh), from the
# composition KICKOS_WITNESS_SYSTEM against an installed KickOS package whose catalogue carries
# tests/drivers.

cmake_minimum_required(VERSION 3.24)
project(kickos_console_witness LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)

set(KICKOS_WITNESS_SYSTEM "" CACHE FILEPATH "The witness's composition")
if(NOT EXISTS "${KICKOS_WITNESS_SYSTEM}")
  message(FATAL_ERROR "KICKOS_WITNESS_SYSTEM '${KICKOS_WITNESS_SYSTEM}' names no composition")
endif()

kickos_compose(witness_system "${KICKOS_WITNESS_SYSTEM}")
add_executable(console_witness writer.cc)
target_link_libraries(console_witness PRIVATE KickOS::kernel witness_system)
kickos_emit_image(console_witness)
