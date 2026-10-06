# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The plain app of M10.5_smallest_boards_meas.md, added to user/apps/common/ of the measured tree:
# A on the `kickos` leaf, B on the `kickos_cxx` leaf, C on the board's default composition. The
# two leaves are gone from later trees.
add_executable(m1055_a main.c)
target_link_libraries(m1055_a PRIVATE kickos)
kickos_emit_image(m1055_a)

add_executable(m1055_b main.c)
target_link_libraries(m1055_b PRIVATE kickos_cxx)
kickos_emit_image(m1055_b)

add_executable(m1055_c main.c)
target_link_libraries(m1055_c PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(m1055_c)

# The system target each selftest image under C linked in place of the two leaves, for size only.
kickos_compose(m1055_selftest_system "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_BOARD}.yaml")
