# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(faultsurvive main.cc)
target_link_libraries(faultsurvive PRIVATE kickos)
kickos_emit_image(faultsurvive)
target_compile_definitions(faultsurvive PRIVATE KICKOS_FS_MODE=0)

# Built everywhere the app is, so all three #if branches of main.cc stay compiled.
kickos_add_app_target(faultsurvive_ovf main.cc)
target_link_libraries(faultsurvive_ovf PRIVATE kickos)
kickos_emit_image(faultsurvive_ovf)
target_compile_definitions(faultsurvive_ovf PRIVATE KICKOS_FS_MODE=1)

# Mode 2 moves SP itself, so it needs the ISA's spelling for it; main.cc #errors rather than
# degrading if this guard is loosened.
if(NOT (KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "x86_64"))
  kickos_add_app_target(faultsurvive_off main.cc)
  target_link_libraries(faultsurvive_off PRIVATE kickos)
  kickos_emit_image(faultsurvive_off)
  target_compile_definitions(faultsurvive_off PRIVATE KICKOS_FS_MODE=2)
endif()

# The trap-stack security regression: an unprivileged thread aims its SP at a kernel word and
# traps. Only rv32imac and rxv3 have a SOFTWARE trap prologue to fool; main.cc #errors if an
# image is built anywhere else.
if(KICKOS_ARCH STREQUAL "rv32imac" OR KICKOS_ARCH STREQUAL "rxv3")
  kickos_add_app_target(faultsurvive_kwrite main.cc)
  target_link_libraries(faultsurvive_kwrite PRIVATE kickos)
  kickos_emit_image(faultsurvive_kwrite)
  target_compile_definitions(faultsurvive_kwrite PRIVATE KICKOS_FS_MODE=3)

  # Mode 5 drops SP two bytes: in bounds, deep inside the stack, and misaligned, so only the
  # prologue's alignment leg can refuse it.
  kickos_add_app_target(faultsurvive_misalign main.cc)
  target_link_libraries(faultsurvive_misalign PRIVATE kickos)
  kickos_emit_image(faultsurvive_misalign)
  target_compile_definitions(faultsurvive_misalign PRIVATE KICKOS_FS_MODE=5)
endif()

# Mode 4 parks SP at the low edge of the stack, with less room beneath it than the frame plus the
# kernel descent would need, and expects the kill anyway. It PINS rv32imac's frame and depth
# figures from rv_trap_stack.h; main.cc #errors if an image is built anywhere else.
if(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_add_app_target(faultsurvive_lowedge main.cc)
  target_link_libraries(faultsurvive_lowedge PRIVATE kickos)
  kickos_emit_image(faultsurvive_lowedge)
  target_compile_definitions(faultsurvive_lowedge PRIVATE KICKOS_FS_MODE=4)
endif()

if(KICKOS_ARCH STREQUAL "rv32imac")
  foreach(_fs_tgt faultsurvive_kwrite faultsurvive_lowedge)
    target_include_directories(${_fs_tgt} PRIVATE
      "${PROJECT_SOURCE_DIR}/arch/riscv/rv32imac/include")
  endforeach()
endif()
