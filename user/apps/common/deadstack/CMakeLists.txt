# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(deadstack main.cc)
target_link_libraries(deadstack PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(deadstack)
