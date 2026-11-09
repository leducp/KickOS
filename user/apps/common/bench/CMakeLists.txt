# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(bench main.cc owner.cc)
target_include_directories(bench PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
# The walk slots hold main and the holder beside the waiters, and the first step needs two.
set(BENCH_OD_WAITERS_MAX 0)
if(KICKOS_BENCH_WALK_SLOTS GREATER 0)
  if(KICKOS_BENCH_WALK_SLOTS LESS 4)
    message(FATAL_ERROR "KICKOS_BENCH_WALK_SLOTS is ${KICKOS_BENCH_WALK_SLOTS}: the walk rows need "
      "main, a holder and two waiters at least, so 4 or more, or 0 for no rows")
  endif()
  math(EXPR BENCH_OD_WAITERS_MAX "${KICKOS_BENCH_WALK_SLOTS} - 2")
endif()
target_compile_definitions(bench PRIVATE BENCH_OD_WAITERS_MAX=${BENCH_OD_WAITERS_MAX})
add_executable(bench_peer peer.cc)
target_link_libraries(bench_peer PRIVATE kickos)
# main's THREADS: its entry, the two players and the end-to-end pair, and a spinner on each core
# past the second, at most three; or the walk slots, if more; on the SMP images two IPC peers
# per core.
math(EXPR BENCH_MAIN_THREADS "${KICKOS_KERNEL_CORES} + 3")
if(BENCH_MAIN_THREADS LESS 5)
  set(BENCH_MAIN_THREADS 5)
elseif(BENCH_MAIN_THREADS GREATER 8)
  set(BENCH_MAIN_THREADS 8)
endif()
if(BENCH_MAIN_THREADS LESS KICKOS_BENCH_WALK_SLOTS)
  set(BENCH_MAIN_THREADS ${KICKOS_BENCH_WALK_SLOTS})
endif()
configure_file(system.yaml "${CMAKE_CURRENT_BINARY_DIR}/system.yaml" @ONLY)
math(EXPR BENCH_MAIN_THREADS "2 * ${KICKOS_KERNEL_CORES} + 1")
configure_file(system_smp.yaml "${CMAKE_CURRENT_BINARY_DIR}/system_smp.yaml" @ONLY)
kickos_app_system(bench COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/system.yaml")
kickos_emit_image(bench)

if(KICKOS_ARCH STREQUAL "x86_64" OR
   ((KICKOS_ARCH STREQUAL "armv8a" OR KICKOS_ARCH STREQUAL "lx6") AND
    KICKOS_KERNEL_CORES GREATER 1))
  add_executable(bench_smp smp_lock.cc)
  target_compile_definitions(bench_smp PRIVATE BENCH_MAIN_THREADS=${BENCH_MAIN_THREADS})
  kickos_app_system(bench_smp COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/system_smp.yaml"
                     SYSTEM bench_smp_system)
  if(NOT KICKOS_ARCH STREQUAL "x86_64")
    if(KICKOS_KERNEL_CORES GREATER 4)
      target_compile_definitions(bench_smp PRIVATE KICKOS_SMP_LOCK_ROUNDS=50000)
    else()
      target_compile_definitions(bench_smp PRIVATE KICKOS_SMP_LOCK_ROUNDS=200000)
    endif()
  endif()
  kickos_emit_image(bench_smp)
endif()

if(KICKOS_ARCH STREQUAL "x86_64" OR
   (KICKOS_ARCH STREQUAL "armv8a" AND KICKOS_CONFIG_VARIANT STREQUAL "benchsmp12"))
  add_executable(bench_smp_ipc smp_ipc.cc)
  target_compile_definitions(bench_smp_ipc PRIVATE BENCH_MAIN_THREADS=${BENCH_MAIN_THREADS})
  kickos_app_system(bench_smp_ipc COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/system_smp.yaml"
                     SYSTEM bench_smp_system)
  set(KICKOS_M95_BENCH_PAIRS "${KICKOS_KERNEL_CORES}" CACHE STRING
      "Active pairs in the SMP IPC benchmark")
  set(KICKOS_M95_BENCH_REMOTE_PAIRS "0" CACHE STRING
      "Pairs whose client runs on the next SMP core")
  if(KICKOS_M95_BENCH_PAIRS LESS 1 OR KICKOS_M95_BENCH_PAIRS GREATER KICKOS_KERNEL_CORES)
    message(FATAL_ERROR "KICKOS_M95_BENCH_PAIRS must be between 1 and KICKOS_KERNEL_CORES")
  endif()
  if(KICKOS_M95_BENCH_REMOTE_PAIRS LESS 0 OR
     KICKOS_M95_BENCH_REMOTE_PAIRS GREATER KICKOS_M95_BENCH_PAIRS OR
     (KICKOS_M95_BENCH_REMOTE_PAIRS GREATER 0 AND KICKOS_KERNEL_CORES LESS 2))
    message(FATAL_ERROR "KICKOS_M95_BENCH_REMOTE_PAIRS must fit the pairs and cores")
  endif()
  target_compile_definitions(bench_smp_ipc PRIVATE
    KICKOS_M95_BENCH_PAIRS=${KICKOS_M95_BENCH_PAIRS}
    KICKOS_M95_BENCH_REMOTE_PAIRS=${KICKOS_M95_BENCH_REMOTE_PAIRS})
  if(KICKOS_ARCH STREQUAL "armv8a")
    target_compile_definitions(bench_smp_ipc PRIVATE KICKOS_M95_BENCH_ROUNDS=5000)
  endif()
  kickos_emit_image(bench_smp_ipc)
endif()
