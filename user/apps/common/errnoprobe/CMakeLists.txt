# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "x86_64")
  return()
endif()

kickos_add_application(errnoprobe
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_errnoprobe TARGET errnoprobe
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_errnoprobe TARGET errnoprobe BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh")
endif()

# Same decline as the arm64 registration below, and this board is where it stopped being an
# argument. The comment there said registering above one kernel core "would assert a gate that
# can never pass"; that claim had never been TESTED, because arm64 does not register the test
# above one core and no other four-core board registered it at all. This branch built the first
# four-core RV64 board, ran it, and got the seat's own failure, then ran the same app on the
# four-core arm64 preset and got the identical one. The claim is reproduced on two arches now
# rather than reasoned, and the fix STATE.md names is unchanged: the seat belongs in
# thread-local storage.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64" AND KICKOS_KERNEL_CORES EQUAL 1)
  kickos_add_qemu_test(NAME qemu_riscv64_errnoprobe TARGET errnoprobe BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh")
endif()

# NOT registered above one kernel core, and that is a claim rather than a convenience: the
# reentrancy seat is one word in the process's memory, so two threads of one process on two cores
# resolve the same errno, and no per-core keying reaches a word read at EL0. Registering it there
# would assert a gate that can never pass, which a run-time skip then has to explain on every
# sweep. The app still BUILDS on that posture, so the code stays compiled.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64" AND KICKOS_KERNEL_CORES EQUAL 1)
  kickos_add_qemu_test(NAME qemu_arm64_errnoprobe TARGET errnoprobe BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh"
    ARGS ${KICKOS_KERNEL_CORES})
endif()
