# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(libc_exit main.cc)
target_link_libraries(libc_exit PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(libc_exit)
