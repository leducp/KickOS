# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Judged only from a capture route that stamps each line's arrival on the host.
add_executable(wallclock main.cc)
target_link_libraries(wallclock PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(wallclock)
kickos_app_judge(wallclock tests/integration/check_wallclock.sh)

# The same marks over the board's USB device console, where it has one.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/systems/${KICKOS_BOARD}.yaml")
  add_executable(wallclock_usb main.cc)
  kickos_app_system(wallclock_usb COMPOSITION systems/${KICKOS_BOARD}.yaml)
  kickos_emit_image(wallclock_usb)
  kickos_app_judge(wallclock_usb tests/integration/check_wallclock.sh)
endif()
