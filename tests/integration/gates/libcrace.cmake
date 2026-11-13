# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The C library's lock riding `libcrace`, on every emulated board.

if(NOT TARGET libcrace)
  return()
endif()
if(KICKOS_ARCH STREQUAL "sim")
  string(CONCAT _libcrace_why "the sim runs an instance's threads on one host thread, which "
    "the host libc's stdio lock cannot separate")
  kickos_inapplicable(libcrace "${_libcrace_why}")
  return()
endif()
# The dead malloc holder arms tear the heap they run in, so they run only where that heap is one
# task's own and a fault ends that task alone. A capture judge reads the posture from the cache.
set(_libcrace_dead 0)
if(KICKOS_FAULT_ISOLATION AND KICKOS_HAVE_ASPACE)
  set(_libcrace_dead 1)
endif()
set(KICKOS_LIBCRACE_DEAD_ARMS ${_libcrace_dead} CACHE INTERNAL
    "libcrace runs its dead malloc holder arms")
kickos_app_judge(libcrace tests/integration/check_libcrace.sh)
# The emulator options an image linked for another memory than the chip's needs.
get_target_property(_libcrace_qemu libcrace KICKOS_IMAGE_QEMU_ARGS)
if(NOT _libcrace_qemu)
  set(_libcrace_qemu "")
endif()
set(_libcrace_qemu_args)
if(_libcrace_qemu)
  set(_libcrace_qemu_args QEMU_ARGS ${_libcrace_qemu})
endif()
kickos_add_qemu_test(TARGET libcrace
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libcrace.sh"
  ARGS ${_libcrace_dead} ${_libcrace_qemu_args})
# The same run with an interrupt deliverable between any two instructions: free-running TCG
# delivers one only between translation blocks, which hides a window inside a block.
if(NOT TARGET libcrace_any_insn)
  return()
endif()
kickos_emulator_judged(libcrace_any_insn)
get_target_property(_libcrace_qemu libcrace_any_insn KICKOS_IMAGE_QEMU_ARGS)
if(NOT _libcrace_qemu)
  set(_libcrace_qemu "")
endif()
kickos_add_qemu_test(TARGET libcrace_any_insn
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libcrace.sh"
  ARGS ${_libcrace_dead}
  QEMU_ARGS ${_libcrace_qemu} -accel tcg,one-insn-per-tb=on)
