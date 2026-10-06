# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(aspaceufault main.cc)
target_link_libraries(aspaceufault PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(aspaceufault)
