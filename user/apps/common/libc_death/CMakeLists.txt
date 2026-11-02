# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(libc_death main.cc)
target_link_libraries(libc_death PRIVATE KickOS::kickos KickOS::system_default)
kickos_emit_image(libc_death)
