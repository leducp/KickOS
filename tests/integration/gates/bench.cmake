# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Benchmark tests require the bench target. Guard properties with if(TEST)
# because boards without an emulator register no image tests.
# Control tests use fixed reports and need no emulator.

if(NOT TARGET bench)
  return()
endif()

# Use kernel-core count: AMP kernels may each own only one machine core.
# Run SMP sweeps serially to avoid host contention exhausting wall-clock timeouts.
if(KICKOS_KERNEL_CORES GREATER 1)
  kickos_add_qemu_test(NAME ${_tag}_bench_percore TARGET bench
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_percore.sh"
    ARGS ${KICKOS_KERNEL_CORES}
    TIMEOUT 240)
  if(TEST ${_tag}_bench_percore)
    set_tests_properties(${_tag}_bench_percore PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

add_test(NAME ${_tag}_bench_percore_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_percore.sh" --controls)
kickos_host_gate(${_tag}_bench_percore_controls)

# Check phase-table completeness at every core count.
kickos_add_qemu_test(NAME ${_tag}_bench_phase_table TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_phase_table.sh"
  TIMEOUT 240)
if(TEST ${_tag}_bench_phase_table)
  set_tests_properties(${_tag}_bench_phase_table PROPERTIES RUN_SERIAL TRUE)
endif()

add_test(NAME ${_tag}_bench_phase_table_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_phase_table.sh" --controls)
kickos_host_gate(${_tag}_bench_phase_table_controls)

# Inspect per-core switch timestamp addressing in the linked image.
# Runtime tests cannot expose sharing because the kernel lock serializes the bracket.
if(KICKOS_KERNEL_CORES GREATER 1)
  set(_bench_image "$<TARGET_FILE:bench>")
  if(KICKOS_ARCH STREQUAL "x86_64")
    get_target_property(_bench_image bench KICKOS_IMAGE_FILE)
  endif()
  add_test(NAME ${_tag}_bench_stamp_percore
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_bench_stamp_percore.sh"
            "${_bench_image}" "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}")
  kickos_host_gate(${_tag}_bench_stamp_percore)
endif()

# Check SMP release/acquire instructions where the architecture has distinct acquire/release
# instructions. x86 TSO uses the same MOV for relaxed, acquire and release; disassembly cannot
# distinguish a source-level downgrade there. Its runtime E2E gate still runs above one core.
if(KICKOS_KERNEL_CORES GREATER 1 AND NOT KICKOS_ARCH STREQUAL "x86_64")
  add_test(NAME ${_tag}_bench_e2e_publish
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_bench_e2e_publish.sh"
            "${_bench_image}" "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}")
  kickos_host_gate(${_tag}_bench_e2e_publish)
endif()

# Check that LX6 consumes its delayed switch-end timestamp at every core count.
if(KICKOS_ARCH STREQUAL "lx6")
  add_test(NAME ${_tag}_bench_xtensa_stamp
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_bench_xtensa_stamp.sh"
            "$<TARGET_FILE:bench>" "${CMAKE_OBJDUMP}")
  kickos_host_gate(${_tag}_bench_xtensa_stamp)
endif()

# Check PMCR_EL0.LC in the linked image. QEMU returns a 64-bit cycle counter
# even when this bit is missing.
if(KICKOS_ARCH STREQUAL "armv8a")
  add_test(NAME ${_tag}_bench_a53_pmcr
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_bench_a53_pmcr.sh"
            "$<TARGET_FILE:bench>" "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}")
  kickos_host_gate(${_tag}_bench_a53_pmcr)
endif()

# Check outermost lock sampling at every core count; WAIT exists only on SMP.
kickos_add_qemu_test(NAME ${_tag}_bench_lock TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_lock.sh"
  ARGS ${KICKOS_KERNEL_CORES}
  TIMEOUT 300)
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
    ARGS ${KICKOS_KERNEL_CORES}
    TIMEOUT 300)
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
  ARGS ${KICKOS_KERNEL_CORES}
  TIMEOUT 480)
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
    ARGS benchauth 22
    TIMEOUT 240)
endif()
