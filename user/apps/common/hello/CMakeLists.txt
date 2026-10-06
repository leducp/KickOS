# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(hello main.cc)
target_link_libraries(hello PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(hello)
