# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Judged only from a capture route that stamps each line's arrival on the host.
add_executable(wallclock main.cc)
target_link_libraries(wallclock PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(wallclock)
kickos_app_judge(wallclock tests/integration/check_wallclock.sh)
