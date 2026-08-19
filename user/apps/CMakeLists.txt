# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Board-agnostic + multi-board apps (each self-gates on its own condition).
add_subdirectory(common)

# Board-specific apps: the directory IS the board gate. Boards without a
# per-board app subdir (sim, qemu, microbit, qemu-riscv, picopi, blackpill,
# due, ...) simply skip; add_subdirectory on a missing dir is a hard error.
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_BOARD}")
  add_subdirectory("${KICKOS_BOARD}")
endif()
