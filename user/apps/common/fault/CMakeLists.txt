# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_diagnostic_app(fault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET fault)
  return()
endif()

# 139 is KOS_EXIT_FAULT (user/include/kickos/sys/abi.h) and 132 kfault_terminate's, both restated
# here on purpose: a gate computing the number from the same header the runtime reports would
# assert nothing about it.
if(KICKOS_FAULT_ISOLATION)
  set(_fault_marker "THREAD FAULT")
  set(_fault_status 139)
else()
  set(_fault_status 132)
  if(KICKOS_ARCH STREQUAL "sim")
    set(_fault_marker "SIM FAULT")
  elseif(KICKOS_ARCH STREQUAL "rv32imac")
    set(_fault_marker "RISC-V TRAP")
  elseif(KICKOS_ARCH STREQUAL "armv8a")
    set(_fault_marker "ARMV8A EXCEPTION")
  else()
    set(_fault_marker "HARD FAULT")
  endif()
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME fault_dump
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh" "$<TARGET_FILE:fault>"
            "${_fault_marker}" ${_fault_status})
  set_tests_properties(fault_dump PROPERTIES TIMEOUT 15)
endif()

# check_fault_dump.sh runs NATIVELY unless QEMU_MACHINE is set, so kickos_add_qemu_test must pass
# the per-board machine name.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_fault_dump TARGET fault BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_fault_marker}" ${_fault_status})
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_fault_dump TARGET fault BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_fault_marker}" ${_fault_status})
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_fault_dump TARGET fault BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_fault_marker}" ${_fault_status})
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_fault_dump TARGET fault BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_fault_marker}" ${_fault_status})
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_fault_dump TARGET fault BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_fault_marker}" ${_fault_status})
endif()

# 139 does not survive isa-debug-exit, which reports (status << 1) | 1 into an 8-bit exit code;
# the image prints its status on the console instead and tests/lib/gate.sh prefers that line
# (arch/x86/chip/q35/chip_q35.cc, arch_shutdown).
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-x86_64")
  kickos_add_qemu_test(NAME qemu_x86_64_fault_dump TARGET fault BOARD qemu-x86_64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_fault_marker}" ${_fault_status})
endif()
