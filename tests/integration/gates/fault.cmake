# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `fault`: the dump a deliberate memory violation leaves, read out of a native
# run on the sim and out of every emulator board.

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

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME fault_dump
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh" "$<TARGET_FILE:fault>"
            "${_fault_marker}" ${_fault_status})
  set_tests_properties(fault_dump PROPERTIES TIMEOUT 15)
endif()

# check_fault_dump.sh runs NATIVELY unless QEMU_MACHINE is set, so kickos_add_qemu_test must pass
# the per-board machine name.
#
# Every board with an emulator takes this same call, x86_64 included. There 139 does not survive
# isa-debug-exit, which reports (status << 1) | 1 into an 8-bit exit code; the image prints its
# status on the console instead and tests/lib/gate.sh prefers that line
# (arch/x86/chip/q35/chip_q35.cc, arch_shutdown).
kickos_add_qemu_test(NAME ${_tag}_fault_dump TARGET fault
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
  ARGS "${_fault_marker}" ${_fault_status})
