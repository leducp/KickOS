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
    FAIL_REGULAR_EXPRESSION "ERROR|did not fault|cross-domain write completed")
endif()

# QEMU RISC-V PMP-enforcement gate: the SAME isolation demo on a QEMU `virt`
# (RV32IMAC, M-mode), registered only when the build enforces (HAVE_MPU=1) -- on a
# no-enforcement build the cross-domain write completes, which is correct there.
# `-bios none` runs the image bare-metal in M-mode.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv" AND KICKOS_HAVE_MPU)
  kickos_add_qemu_test(NAME qemu_riscv_mpu_fault TARGET mpu_fault BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_mpu_fault.sh")
endif()

# The ARM peer of that gate: proof that a PMSA denial actually TRAPS at runtime, not
# merely that the enforcement layout links (every other enforcing ARM port is
# silicon). The PMSAv7 images exercise the shared armv7m commit; qemu-m33 the
# dedicated PMSAv8 backend, whose RBAR/RLAR encoding is a different register layout.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS AND KICKOS_HAVE_MPU)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_mpu_fault TARGET mpu_fault BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_mpu_fault.sh")
endif()
