# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(sched_exit main.cc)
target_link_libraries(sched_exit PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(sched_exit)
