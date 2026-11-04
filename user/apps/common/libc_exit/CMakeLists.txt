# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(libc_exit main.cc)
target_link_libraries(libc_exit PRIVATE KickOS::kickos KickOS::system_default)
kickos_emit_image(libc_exit)

# The sim's abort() is the host's, which ends the host process.
if(NOT KICKOS_ARCH STREQUAL "sim")
  add_executable(libc_exit_abort main.cc)
  target_compile_definitions(libc_exit_abort PRIVATE KICKOS_LIBC_EXIT_ABORT)
  target_link_libraries(libc_exit_abort PRIVATE KickOS::kickos KickOS::system_default)
  kickos_emit_image(libc_exit_abort)
endif()
