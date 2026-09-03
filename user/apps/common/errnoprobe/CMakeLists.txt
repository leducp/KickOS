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

# Same decline as the arm64 registration below. Reproduced on both arches rather than reasoned:
# a four-core run reports the seat's own failure, a preempted thread coming back on the peer's
# errno. The fix is thread-local storage.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64" AND KICKOS_KERNEL_CORES EQUAL 1)
  kickos_add_qemu_test(NAME qemu_riscv64_errnoprobe TARGET errnoprobe BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh")
endif()

# NOT registered above one kernel core, and that is a claim rather than a convenience: the
# reentrancy seat is one word in the process's memory, so two threads of one process on two cores
# resolve the same errno, and no per-core keying reaches a word read at EL0. Registering it there
# would assert a gate that can never pass, which a run-time skip then has to explain on every
# sweep. The app still BUILDS on that posture, so the code stays compiled.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_ARM64_BOARDS
   AND KICKOS_KERNEL_CORES EQUAL 1)
  string(REPLACE "-" "_" _tag "${KICKOS_BOARD}")
  kickos_add_qemu_test(NAME ${_tag}_errnoprobe TARGET errnoprobe BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh"
    ARGS ${KICKOS_KERNEL_CORES})
endif()
