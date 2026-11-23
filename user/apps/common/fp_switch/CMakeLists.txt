# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(fp_switch main.cc)
if(KICKOS_ARCH STREQUAL "armv7m")
  target_sources(fp_switch PRIVATE fresh.cc)
  # The reader's own code may hold nothing in an FP register, or it masks the leak it reads.
  # The hard float ABI refuses the flag on any header declaring a float.
  if(NOT KICKOS_MFLOAT_ABI STREQUAL "hard")
    set_source_files_properties(fresh.cc PROPERTIES COMPILE_OPTIONS -mgeneral-regs-only)
  endif()
endif()
target_link_libraries(fp_switch PRIVATE KickOS::kickos KickOS::system_default)
kickos_emit_image(fp_switch)
