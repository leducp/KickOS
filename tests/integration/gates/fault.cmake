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
# The RX image traps with BRK, which shares the catch-all vector with the NMI and so is never a
# thread fault, isolating or not.
if(KICKOS_ARCH STREQUAL "rxv3")
  set(_fault_marker "RX EXCEPTION (trap)")
  set(_fault_status 132)
elseif(KICKOS_FAULT_ISOLATION)
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
  elseif(KICKOS_ARCH STREQUAL "armv7m")
    set(_fault_marker "USAGE FAULT")
  else()
    set(_fault_marker "HARD FAULT")
  endif()
endif()

if(KICKOS_FAULT_ISOLATION OR KICKOS_ARCH MATCHES "^(sim|rv32imac|armv8a|armv7m|armv6m|rxv3)$")
  kickos_app_judge(fault tests/integration/check_fault_dump.sh ARGS "${_fault_marker}")
else()
  kickos_emulator_judged(fault)
endif()
if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME fault_dump
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh" "$<TARGET_FILE:fault>"
            "${_fault_marker}" ${_fault_status})
  kickos_boot_timeout(fault_dump "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh")
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

# The divide by zero where no fault is isolated: the dump names the division, and the marker is
# that name rather than the banner, which every fault on these cores shares.
foreach(_fault_div fault_div0 fault_ldiv0)
  if(NOT TARGET ${_fault_div})
    continue()
  endif()
  if(KICKOS_ARCH STREQUAL "armv6m")
    set(_div_marker "DIVBYZERO: the UDF of __aeabi_idiv0/__aeabi_ldiv0")
  elseif(KICKOS_ARCH STREQUAL "lx6")
    set(_div_marker "XTENSA EXCEPTION (integer divide by zero)")
  else()
    message(FATAL_ERROR "${_fault_div}: no divide-by-zero dump is defined for ${KICKOS_ARCH}")
  endif()
  kickos_app_judge(${_fault_div} tests/integration/check_fault_dump.sh ARGS "${_div_marker}")
  kickos_add_qemu_test(NAME ${_tag}_${_fault_div} TARGET ${_fault_div}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fault_dump.sh"
    ARGS "${_div_marker}" 132)
endforeach()
