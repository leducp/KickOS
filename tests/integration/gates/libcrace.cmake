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
# Above one core a host CPU hog preempts the vCPU holding the kernel lock and the others wait on
# it, so a loaded run has taken 393 s on x86_64 and 324 s on arm64 GICv2. The floor yields to a
# larger QEMU_TIMEOUT.
set(_libcrace_bound)
if(KICKOS_NUM_CORES GREATER 1)
  set(_libcrace_bound 900)
  if(NOT "$ENV{QEMU_TIMEOUT}" STREQUAL "" AND "$ENV{QEMU_TIMEOUT}" GREATER _libcrace_bound)
    set(_libcrace_bound "$ENV{QEMU_TIMEOUT}")
  endif()
  set(_libcrace_bound BOUND ${_libcrace_bound})
endif()
kickos_add_qemu_test(TARGET libcrace
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libcrace.sh"
  ARGS ${_libcrace_dead} ${_libcrace_bound})
