# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(bench main.cc)
target_link_libraries(bench PRIVATE kickos)
kickos_emit_image(bench)

if(KICKOS_ARCH STREQUAL "x86_64" OR
   ((KICKOS_ARCH STREQUAL "armv8a" OR KICKOS_ARCH STREQUAL "lx6") AND
    KICKOS_KERNEL_CORES GREATER 1))
  kickos_add_app_target(bench_smp smp_lock.cc)
  target_link_libraries(bench_smp PRIVATE kickos)
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
  kickos_add_app_target(bench_smp_ipc smp_ipc.cc)
  target_link_libraries(bench_smp_ipc PRIVATE kickos)
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
  # Root holds one endpoint per core; each server holds one reply cap at a time.
  kickos_declare_app_capabilities(bench_smp_ipc ${KICKOS_M95_BENCH_PAIRS} 0 1)
  kickos_emit_image(bench_smp_ipc)
endif()
