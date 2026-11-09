# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# One image per access, each from main's own unprivileged thread.
foreach(_arm unaligned stir div0)
  add_executable(ccrprobe_${_arm} main.cc)
  target_link_libraries(ccrprobe_${_arm} PRIVATE KickOS::kickos KickOS::system_default)
  string(TOUPPER "${_arm}" _arm_def)
  target_compile_definitions(ccrprobe_${_arm} PRIVATE CCRPROBE_${_arm_def}=1)
  kickos_emit_image(ccrprobe_${_arm})
endforeach()
