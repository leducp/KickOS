# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_apps_bootloader: a panic, a fault no task survives and every ordered end hand the board to
# the chip's bootloader.
add_library(kickos_apps_terminal OBJECT terminal.cc)
# An archive, not an object: the wrap is every image's, and each image asking a shutdown takes the
# definition from it.
add_library(kickos_apps_shutdown STATIC shutdown.cc)
foreach(_t kickos_apps_terminal kickos_apps_shutdown)
  kickos_apply_freestanding(${_t})
  target_link_libraries(${_t} PRIVATE kickos_user kickos_system)
endforeach()

add_library(kickos_apps_bootloader INTERFACE)
target_link_libraries(kickos_apps_bootloader INTERFACE
  "$<TARGET_OBJECTS:kickos_apps_terminal>" kickos_apps_shutdown)
target_link_options(kickos_apps_bootloader INTERFACE "LINKER:--wrap=kos_shutdown")
