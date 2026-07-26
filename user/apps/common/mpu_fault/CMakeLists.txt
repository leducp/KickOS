# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The memory-domain isolation gate, in its own binary (it ends the process on
# the deliberate cross-domain fault). Diagnostic app: built only under
# KICKOS_ENABLE_SELFTEST (it deliberately faults; not a production image).
kickos_add_diagnostic_app(mpu_fault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET mpu_fault)
  return() # diagnostics off -> app not built; skip its tests
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME mpu_fault COMMAND "$<TARGET_FILE:mpu_fault>")
  set_tests_properties(mpu_fault PROPERTIES
    TIMEOUT 15
    PASS_REGULAR_EXPRESSION "MPU FAULT: task 'domainA'"
    FAIL_REGULAR_EXPRESSION "ERROR|did not fault")
endif()

# QEMU RISC-V PMP-enforcement gate: the SAME isolation demo on a QEMU `virt`
# (RV32IMAC, M-mode), registered only when the build enforces (HAVE_MPU=1) -- on a
# no-enforcement build the cross-domain write completes, which is correct there.
# `-bios none` runs the image bare-metal in M-mode.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv" AND KICKOS_HAVE_MPU)
  kickos_add_qemu_test(NAME qemu_riscv_mpu_fault TARGET mpu_fault BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_mpu_fault.sh")
endif()

# The ARM peer of that gate, and the thing ARM enforcement did not have until the mps2
# chip grew an .appdata window: proof that a PMSAv7 denial actually TRAPS at runtime,
# not merely that the enforcement layout links. Every other enforcing ARM port is
# silicon, so build-boards-mpu could only ever check the link surface.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu" AND KICKOS_HAVE_MPU)
  kickos_add_qemu_test(NAME qemu_mpu_fault TARGET mpu_fault BOARD qemu
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_mpu_fault.sh")
endif()
