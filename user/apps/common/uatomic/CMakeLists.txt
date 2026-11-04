# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(uatomic main.cc)
target_link_libraries(uatomic PRIVATE KickOS::kickos KickOS::system_default)
kickos_emit_image(uatomic)
