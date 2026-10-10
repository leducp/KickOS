# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(faultsurvive main.cc)
target_link_libraries(faultsurvive PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(faultsurvive)
target_compile_definitions(faultsurvive PRIVATE KICKOS_FS_MODE=0)

# Not the sim, which maps no guard under a thread's stack, so the recursion overruns main's.
if(NOT KICKOS_ARCH STREQUAL "sim")
  add_executable(faultsurvive_ovf main.cc)
  target_link_libraries(faultsurvive_ovf PRIVATE KickOS::kernel KickOS::system_default)
  kickos_emit_image(faultsurvive_ovf)
  target_compile_definitions(faultsurvive_ovf PRIVATE KICKOS_FS_MODE=1)
endif()
# The survive arm again, its stdout the packaged simcon its composition names.
if(KICKOS_ARCH STREQUAL "sim")
  add_executable(faultsurvive_published main.cc)
  kickos_app_system(faultsurvive_published COMPOSITION system.yaml)
  kickos_emit_image(faultsurvive_published)
  target_compile_definitions(faultsurvive_published PRIVATE KICKOS_FS_MODE=0)
endif()

# Mode 2 moves SP itself, so it needs the ISA's spelling for it; main.cc #errors rather than
# degrading if this guard is loosened.
if(NOT (KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "x86_64"))
  add_executable(faultsurvive_off main.cc)
  target_link_libraries(faultsurvive_off PRIVATE KickOS::kernel KickOS::system_default)
  kickos_emit_image(faultsurvive_off)
  target_compile_definitions(faultsurvive_off PRIVATE KICKOS_FS_MODE=2)
endif()

# The trap-stack security regression: an unprivileged thread aims its SP at a word of main's data
# and traps. Only rv32imac and rxv3 have a SOFTWARE trap prologue to fool; main.cc #errors if an
# image is built anywhere else.
if(KICKOS_ARCH STREQUAL "rv32imac" OR KICKOS_ARCH STREQUAL "rxv3")
  add_executable(faultsurvive_kwrite main.cc)
  target_link_libraries(faultsurvive_kwrite PRIVATE KickOS::kernel KickOS::system_default)
  kickos_emit_image(faultsurvive_kwrite)
  target_compile_definitions(faultsurvive_kwrite PRIVATE KICKOS_FS_MODE=3)

  # Mode 5 drops SP two bytes: in bounds, deep inside the stack, and misaligned, so only the
  # prologue's alignment leg can refuse it.
  add_executable(faultsurvive_misalign main.cc)
  target_link_libraries(faultsurvive_misalign PRIVATE KickOS::kernel KickOS::system_default)
  kickos_emit_image(faultsurvive_misalign)
  target_compile_definitions(faultsurvive_misalign PRIVATE KICKOS_FS_MODE=5)
endif()

# Mode 4 parks SP at the low edge of the stack, with less room beneath it than the frame plus the
# kernel descent would need, and expects the kill anyway. It PINS rv32imac's frame and depth
# figures from rv_trap_stack.h; main.cc #errors if an image is built anywhere else.
if(KICKOS_ARCH STREQUAL "rv32imac")
  add_executable(faultsurvive_lowedge main.cc)
  target_link_libraries(faultsurvive_lowedge PRIVATE KickOS::kernel KickOS::system_default)
  kickos_emit_image(faultsurvive_lowedge)
  target_compile_definitions(faultsurvive_lowedge PRIVATE KICKOS_FS_MODE=4)
endif()

if(KICKOS_ARCH STREQUAL "rv32imac")
  foreach(_fs_tgt faultsurvive_kwrite faultsurvive_lowedge)
    target_include_directories(${_fs_tgt} PRIVATE
      "${PROJECT_SOURCE_DIR}/arch/riscv/rv32imac/include")
  endforeach()
endif()

# Mode 6 aims SP where no memory answers, which only an emulator's map pins: 0xF0000000 is in
# the vendor system space, unimplemented on every mps2 machine.
if(KICKOS_ARCH STREQUAL "armv7m" AND KICKOS_QEMU_MPS2 AND KICKOS_MEMORY_ENFORCED)
  add_executable(faultsurvive_unread main.cc)
  target_link_libraries(faultsurvive_unread PRIVATE KickOS::kernel KickOS::system_default)
  kickos_emit_image(faultsurvive_unread)
  target_compile_definitions(faultsurvive_unread PRIVATE KICKOS_FS_MODE=6
                                                         KICKOS_FS_NOWHERE=0xF0000000u)
endif()
