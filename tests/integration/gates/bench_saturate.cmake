# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Check benchmark saturation on boards with an emulator.
if(NOT TARGET bench)
  return()
endif()

# Allow time for the call/reply sweep before the phase table.
kickos_add_qemu_test(NAME ${_tag}_bench_saturate TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_saturate.sh"
  TIMEOUT 300)

# Run SMP sweeps serially to avoid host contention exhausting the timeout.
if(KICKOS_KERNEL_CORES GREATER 1)
  if(TEST ${_tag}_bench_saturate)
    set_tests_properties(${_tag}_bench_saturate PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

# Test both tick widths with fixed reports on every board. No emulator or readelf needed.
add_test(NAME ${_tag}_bench_saturate_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_saturate.sh" --controls)
kickos_host_gate(${_tag}_bench_saturate_controls)
