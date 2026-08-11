# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The automated fault-dump gate: a deliberate illegal instruction whose arch reporter must
# print a dump and exit 132. Own binary (ends the process). Diagnostic app: built only
# under KICKOS_ENABLE_SELFTEST, so never in a production image.
kickos_add_diagnostic_app(fault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET fault)
  return() # diagnostics off -> app not built; skip its tests
endif()

# Which dump the illegal instruction produces, and the status that goes with it. `fault`
# faults from ROOT, and kmain spawns root unprivileged in every posture, so an isolating
# backend kills the thread instead of panicking; root being the only live thread,
# exit_current then ends the process with the thread's exit code.
#
# 139 is KOS_EXIT_FAULT (user/include/kickos/sys/abi.h) and 132 is kfault_terminate's,
# both restated here on purpose: a gate that computed the number from the same header the
# runtime reports would assert nothing about it.
if(KICKOS_FAULT_ISOLATION)
  set(_fault_marker "THREAD FAULT")
  set(_fault_status 139)
else()
  set(_fault_status 132)
  if(KICKOS_ARCH STREQUAL "sim")
    set(_fault_marker "SIM FAULT")
  elseif(KICKOS_ARCH STREQUAL "rv32imac")
    set(_fault_marker "RISC-V TRAP")
  else()
    set(_fault_marker "HARD FAULT")
  endif()
endif()

# sim: x86 ud2 -> SIGILL -> the sim fault reporter, dumped through the ARMED ring.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME fault_dump
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh" "$<TARGET_FILE:fault>"
            "${_fault_marker}" ${_fault_status})
  set_tests_properties(fault_dump PROPERTIES TIMEOUT 15)
endif()

# qemu (armv7m): udf -> HardFault -> kickos_armv7m_fault_report, on each MPS2 image.
# check_fault_dump.sh runs NATIVELY unless QEMU_MACHINE is set, so kickos_add_qemu_test
# must pass the per-board machine name.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_fault_dump TARGET fault BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_fault_marker}" ${_fault_status})
endif()

# microbit (armv6m): the same, exercising the ldr+bx reporter branch end-to-end.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_fault_dump TARGET fault BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_fault_marker}" ${_fault_status})
endif()

# qemu-riscv (rv32imac): illegal instruction -> kickos_rv_fault_report.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_fault_dump TARGET fault BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_fault_marker}" ${_fault_status})
endif()
