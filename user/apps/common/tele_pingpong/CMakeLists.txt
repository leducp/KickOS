# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(tele_pingpong main.cc)
target_link_libraries(tele_pingpong PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(tele_pingpong)
