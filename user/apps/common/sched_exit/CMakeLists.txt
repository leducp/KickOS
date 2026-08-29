# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(sched_exit
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# The wait-until-last phase can only complete in an image whose service list spawns no
# driver threads: a driver never exits, so kernel().live never reaches 1 and root parks
# forever. main() skips that phase when SCHED_EXIT_SERVICE_THREADS is 1, and the gate
# registrations below stay conditioned on the same value: the markers that phase prints
# are what they assert.
set(_sched_exit_gateable TRUE)
set(_sched_exit_services 0)
if(KICKOS_SERVICE_LIST AND NOT KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  set(_sched_exit_gateable FALSE)
  set(_sched_exit_services 1)
endif()
target_compile_definitions(sched_exit PRIVATE
  SCHED_EXIT_SERVICE_THREADS=${_sched_exit_services})

# The non-last-exit bug was ARM-only (deferred PendSV switch); the sim gate locks parity
# (root's exit arm is arch-neutral and runs everywhere this does). Through the
# same script as the QEMU boards: PASS_REGULAR_EXPRESSION is an OR, so it cannot require
# the worker markers TOO, and root survives an exit that never happened.
if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_sched_exit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh" "$<TARGET_FILE:sched_exit>")
  set_tests_properties(sim_sched_exit PROPERTIES TIMEOUT 15)
endif()

# Multi-instance gate. Registered from HERE because it runs this app, which keeps no
# file-scope counter of its own, so a co-resident run is comparable line for line against
# a solo one. Registered only in the SINGLE-instance tree: the gate configures its own
# tree with the knob on and would otherwise recurse.
if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND NOT KICKOS_MULTI_INSTANCE)
  add_test(NAME sim_multi_instance
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_multi_instance.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_multi_instance PROPERTIES TIMEOUT 300)
endif()

# QEMU armv7m gate: this is where the bug lived (deferred PendSV under IrqLock).
# Run on every MPS2 image: the deferred switch is the same code on each, but the
# exception-return path it rides differs per core.
if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_sched_exit TARGET sched_exit BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")
endif()

# QEMU armv6m gate: same deferred-switch mechanism on Cortex-M0.
if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_sched_exit TARGET sched_exit BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")
endif()

# QEMU rv32imac gate: the same exit path over the RISC-V msip deferred switch.
if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_sched_exit TARGET sched_exit BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")
endif()

# QEMU rv64imac gate: the same exit path over a switch that is synchronous in thread context
# and booked from ISR context, which is the pairing the ARM bug lived in.
if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_sched_exit TARGET sched_exit BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")
endif()
