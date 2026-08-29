# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(sched_exit
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# The wait-until-last phase can only complete in an image whose service list spawns no driver
# threads: a driver never exits, so kernel().live never reaches 1 and root parks forever.
# main() skips that phase when SCHED_EXIT_SERVICE_THREADS is 1, and the gate registrations
# below stay conditioned on the same value.
set(_sched_exit_gateable TRUE)
set(_sched_exit_services 0)
if(KICKOS_SERVICE_LIST AND NOT KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  set(_sched_exit_gateable FALSE)
  set(_sched_exit_services 1)
endif()
target_compile_definitions(sched_exit PRIVATE
  SCHED_EXIT_SERVICE_THREADS=${_sched_exit_services})

# Through the same script as the QEMU boards: PASS_REGULAR_EXPRESSION is an OR, so it cannot
# require the worker markers too, and root survives an exit that never happened.
if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_sched_exit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh" "$<TARGET_FILE:sched_exit>")
  set_tests_properties(sim_sched_exit PROPERTIES TIMEOUT 15)
endif()

# Registered only in the SINGLE-instance tree: the gate configures its own tree with the knob
# on and would otherwise recurse.
if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND NOT KICKOS_MULTI_INSTANCE)
  add_test(NAME sim_multi_instance
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_multi_instance.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_multi_instance PROPERTIES TIMEOUT 300)
endif()

if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_sched_exit TARGET sched_exit BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")
endif()

if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_sched_exit TARGET sched_exit BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")
endif()

if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_sched_exit TARGET sched_exit BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")
endif()

if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_sched_exit TARGET sched_exit BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")
endif()

if(_sched_exit_gateable AND KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-x86_64")
  kickos_add_qemu_test(NAME qemu_x86_64_sched_exit TARGET sched_exit BOARD qemu-x86_64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")
endif()
