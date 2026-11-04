# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(libc_death main.cc)
target_link_libraries(libc_death PRIVATE KickOS::kickos KickOS::system_default)
kickos_emit_image(libc_death)

add_executable(libc_death_env main.cc)
target_compile_definitions(libc_death_env PRIVATE KICKOS_LIBC_DEATH_ENV)
target_link_libraries(libc_death_env PRIVATE KickOS::kickos KickOS::system_default)
kickos_emit_image(libc_death_env)
