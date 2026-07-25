# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The automated fault-dump gate (M2 review item): a deliberate illegal instruction
# whose arch reporter must print a dump and exit 132. Own binary (ends the process).
# Diagnostic app: built only under KICKOS_ENABLE_SELFTEST (never a production image),
# so it is available on every diagnostic/-st build -- including silicon, for on-HW
# fault-dump validation -- without a hand-maintained per-board list.
kickos_add_diagnostic_app(fault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET fault)
  return() # diagnostics off -> app not built; skip its tests
endif()

# sim: x86 ud2 -> SIGILL -> the sim fault reporter, dumped through the ARMED ring.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME fault_dump
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_fault_dump.sh" "$<TARGET_FILE:fault>" "SIM FAULT")
  set_tests_properties(fault_dump PROPERTIES TIMEOUT 15)
endif()

# qemu (armv7m): udf -> HardFault -> kickos_armv7m_fault_report -> exit 132.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  kickos_add_qemu_test(NAME qemu_fault_dump TARGET fault BOARD qemu MACHINE mps2-an386
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_fault_dump.sh" ARGS "HARD FAULT")
endif()

# microbit (armv6m): the same, exercising the ldr+bx reporter branch end-to-end.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_fault_dump TARGET fault BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_fault_dump.sh" ARGS "HARD FAULT")
endif()

# qemu-riscv (rv32imac): illegal instruction -> kickos_rv_fault_report -> exit 132.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_fault_dump TARGET fault BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_fault_dump.sh" ARGS "RISC-V TRAP")
endif()
