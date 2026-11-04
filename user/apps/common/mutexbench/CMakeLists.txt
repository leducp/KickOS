# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Where each task has a space, the window leg's handoff has a waiter of a peer program.
add_executable(mutexbench main.cc)
# main's THREADS: its own, the handoff pair and one load pair, or the board's default where that
# seats fewer.
set(MUTEXBENCH_THREADS 5)
if(KICKOS_BUDGET_THREADS LESS 4)
  set(MUTEXBENCH_THREADS ${KICKOS_BUDGET_THREADS})
endif()
target_compile_definitions(mutexbench PRIVATE MUTEXBENCH_THREADS=${MUTEXBENCH_THREADS})
target_include_directories(mutexbench PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
if(KICKOS_HAVE_ASPACE)
  add_executable(mutexbench_peer peer.cc)
  target_link_libraries(mutexbench_peer PRIVATE kickos)
  configure_file(system_peer.yaml "${CMAKE_CURRENT_BINARY_DIR}/system_peer.yaml" @ONLY)
  kickos_app_system(mutexbench COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/system_peer.yaml")
else()
  configure_file(system.yaml "${CMAKE_CURRENT_BINARY_DIR}/system.yaml" @ONLY)
  kickos_app_system(mutexbench COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/system.yaml")
endif()
kickos_emit_image(mutexbench)
