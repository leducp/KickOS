# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Benchmark tests require the bench target. Guard properties with if(TEST)
# because boards without an emulator register no image tests.
# Control tests use fixed reports and need no emulator.

if(NOT TARGET bench)
  return()
endif()

# Measurements, which the bench drivers named take and no gate judges.
if(TARGET bench_smp)
  kickos_unbooted(bench_smp "a measurement tools/bench/run_m941_smp.sh takes")
endif()
if(TARGET bench_smp_ipc)
  kickos_unbooted(bench_smp_ipc "a measurement tools/bench/run_m95_ipc.sh takes")
endif()

# Use kernel-core count: AMP kernels may each own only one machine core.
# Run SMP sweeps serially to avoid host contention exhausting wall-clock timeouts.
if(KICKOS_KERNEL_CORES GREATER 1)
  kickos_add_qemu_test(NAME ${_tag}_bench_percore TARGET bench
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_percore.sh"
    ARGS ${KICKOS_KERNEL_CORES})
  if(TEST ${_tag}_bench_percore)
    set_tests_properties(${_tag}_bench_percore PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

add_test(NAME ${_tag}_bench_percore_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_percore.sh" --controls)
kickos_host_gate(${_tag}_bench_percore_controls)

# Check phase-table completeness at every core count.
kickos_add_qemu_test(NAME ${_tag}_bench_phase_table TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_phase_table.sh")
if(TEST ${_tag}_bench_phase_table)
  set_tests_properties(${_tag}_bench_phase_table PROPERTIES RUN_SERIAL TRUE)
endif()

add_test(NAME ${_tag}_bench_phase_table_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_phase_table.sh" --controls)
kickos_host_gate(${_tag}_bench_phase_table_controls)

if(KICKOS_ARCH STREQUAL "armv8a")
  kickos_image_rule(a53_pmcr bench)
endif()

# Check outermost lock sampling at every core count; WAIT exists only on SMP.
kickos_add_qemu_test(NAME ${_tag}_bench_lock TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_lock.sh"
  ARGS ${KICKOS_KERNEL_CORES})
if(KICKOS_KERNEL_CORES GREATER 1)
  # Run serially to avoid host contention during the sweep.
  if(TEST ${_tag}_bench_lock)
    set_tests_properties(${_tag}_bench_lock PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

add_test(NAME ${_tag}_bench_lock_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_lock.sh" --controls)
kickos_host_gate(${_tag}_bench_lock_controls)

# Doorbell rounds require more than one kernel core.
if(KICKOS_KERNEL_CORES GREATER 1)
  kickos_add_qemu_test(NAME ${_tag}_bench_doorbell TARGET bench
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_doorbell.sh"
    ARGS ${KICKOS_KERNEL_CORES})
  if(TEST ${_tag}_bench_doorbell)
    set_tests_properties(${_tag}_bench_doorbell PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

add_test(NAME ${_tag}_bench_doorbell_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_doorbell.sh" --controls)
kickos_host_gate(${_tag}_bench_doorbell_controls)

# Kernel-core count determines whether the cross-core IRQ row exists.
kickos_add_qemu_test(NAME ${_tag}_bench_irqspan TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_irqspan.sh"
  ARGS ${KICKOS_KERNEL_CORES})
if(TEST ${_tag}_bench_irqspan)
  set_tests_properties(${_tag}_bench_irqspan PROPERTIES RUN_SERIAL TRUE)
endif()

add_test(NAME ${_tag}_bench_irqspan_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_irqspan.sh" --controls)
kickos_host_gate(${_tag}_bench_irqspan_controls)

# Check KOS_SYS_BENCH authority and bounds. Single-core doorbell calls
# return ENOSYS; the expected test count is unchanged.
if(TARGET benchauth)
  kickos_add_qemu_test(NAME ${_tag}_benchauth TARGET benchauth
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
    ARGS benchauth 22)
endif()
