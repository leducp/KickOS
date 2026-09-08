# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_add_diagnostic_apps(<dir>...)
#   A DIAGNOSTIC app depends on the test-only syscall surface kept out of the production ABI,
#   and/or deliberately faults, so it is built ONLY under KICKOS_ENABLE_SELFTEST. The posture
#   is named HERE, where the tree decides what applies, and not inside the app: that is what
#   lets an app file be its three lines and never ask whether it applies at all.
#
#   Defined in this file rather than in common/ because the per-board parents below need it
#   too, and a command defined in a subdirectory that happens to be added first is a coupling
#   nothing states.
macro(kickos_add_diagnostic_apps)
  if(KICKOS_ENABLE_SELFTEST)
    foreach(_kos_diag_dir ${ARGN})
      add_subdirectory(${_kos_diag_dir})
    endforeach()
  endif()
endmacro()

# Board-agnostic and multi-board apps.
add_subdirectory(common)

# Board-specific apps: the directory IS the board gate. Boards without a
# per-board app subdir (sim, qemu, microbit, qemu-riscv, picopi, blackpill,
# due, ...) simply skip; add_subdirectory on a missing dir is a hard error.
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_BOARD}")
  add_subdirectory("${KICKOS_BOARD}")
endif()
