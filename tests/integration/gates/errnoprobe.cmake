# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The libc reentrancy-seat gate riding `errnoprobe`. The board predicates are load-bearing:
# microbit builds the app and registers no arm, and the arm64 arm is handed the kernel-core
# count where the others are handed nothing.

if(NOT TARGET errnoprobe)
  return()
endif()

set(_errnoprobe_script "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh")

if(KICKOS_CHIP STREQUAL "mps2" OR KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(TARGET errnoprobe SCRIPT "${_errnoprobe_script}")
endif()

# Same decline as the arm64 registration below. Reproduced on both arches rather than reasoned:
# a four-core run reports the seat's own failure, a preempted thread coming back on the peer's
# errno. The fix is thread-local storage.
if(KICKOS_BOARD STREQUAL "qemu-riscv64" AND KICKOS_KERNEL_CORES EQUAL 1)
  kickos_add_qemu_test(TARGET errnoprobe SCRIPT "${_errnoprobe_script}")
endif()

# NOT registered above one kernel core, and that is a claim rather than a convenience: the
# reentrancy seat is one word in the process's memory, so two threads of one process on two
# cores resolve the same errno, and no per-core keying reaches a word read at EL0. Registering
# it there would assert a gate that can never pass, which a run-time skip then has to explain
# on every sweep. The app still BUILDS on that posture, so the code stays compiled.
if(KICKOS_ARCH STREQUAL "armv8a" AND KICKOS_KERNEL_CORES EQUAL 1)
  kickos_add_qemu_test(TARGET errnoprobe SCRIPT "${_errnoprobe_script}"
    ARGS ${KICKOS_KERNEL_CORES})
endif()
