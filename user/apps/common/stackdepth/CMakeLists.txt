# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Two images off one source. Mode 0 panics and nothing else; mode 1 drives the grant-carrying
# spawn first. The block records a maximum, so the difference between the two readings is what
# the spawn arm reached.
foreach(_mode 0 1)
  add_executable(stackdepth${_mode} main.cc)
  target_link_libraries(stackdepth${_mode} PRIVATE KickOS::kernel KickOS::system_default)
  kickos_emit_image(stackdepth${_mode})
  target_compile_definitions(stackdepth${_mode} PRIVATE KICKOS_SD_MODE=${_mode})
endforeach()
