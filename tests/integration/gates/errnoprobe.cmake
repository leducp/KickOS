# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The libc reentrancy-seat gate riding `errnoprobe`.

if(NOT TARGET errnoprobe)
  return()
endif()

set(_errnoprobe_script "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_errnoprobe.sh")

if(NOT KICKOS_ARCH STREQUAL "x86_64")
  kickos_add_qemu_test(TARGET errnoprobe SCRIPT "${_errnoprobe_script}")
endif()

# q35 adds arm F, libc on root, which runs the app's constructors, as a core's first thread, and
# names the core that must have entered it: the boot core at one kernel core, the one core a
# root mask names, and otherwise whichever core picked root first.
if(KICKOS_ARCH STREQUAL "x86_64")
  set(_errnoprobe_core "any")
  if(KICKOS_KERNEL_CORES EQUAL 1)
    set(_errnoprobe_core 0)
  else()
    math(EXPR _mask "${KICKOS_ROOT_CORE_MASK}")
    foreach(_c RANGE 0 31)
      math(EXPR _bit "1 << ${_c}")
      if(_mask EQUAL _bit)
        set(_errnoprobe_core ${_c})
      endif()
    endforeach()
  endif()
  kickos_add_qemu_test(TARGET errnoprobe SCRIPT "${_errnoprobe_script}"
    ARGS --first-core ${_errnoprobe_core})
endif()
