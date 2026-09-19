# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Check the benchmark cycle source on boards with an emulator.
if(NOT TARGET bench)
  return()
endif()

# Allow time for the call/reply sweep and all throughput windows.
kickos_add_qemu_test(NAME ${_tag}_bench_cyccnt TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_cyccnt.sh"
  TIMEOUT 300)

# Run SMP sweeps serially to avoid host contention exhausting the timeout.
if(KICKOS_KERNEL_CORES GREATER 1)
  if(TEST ${_tag}_bench_cyccnt)
    set_tests_properties(${_tag}_bench_cyccnt PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

# Run the same parser with fixed reports on every board, without an emulator.
add_test(NAME ${_tag}_bench_cyccnt_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_cyccnt.sh" --controls)
kickos_host_gate(${_tag}_bench_cyccnt_controls)
