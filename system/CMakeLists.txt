# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kickos_system: home of the fleet-wide system headers (the syscall error taxonomy,
# the frozen capability-index convention, and the init-service seam) plus the
# default userspace init provider.

# INTERFACE library: the header home. Propagated to every app because kickos_core
# links it (root CMakeLists.txt); it contributes no archive, so it must NOT sit in
# the RESCAN link group.
add_library(kickos_system INTERFACE)
target_include_directories(kickos_system INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# The default init provider. Three TUs: the entry (kickos_init_entry), the app-main
# run body (kickos_default_init_run), and the console bring-up step
# (kickos_console_bringup_run, which references the CMake-selected kickos_board_console
# resolved across the rescan link group). A delegating override references only the
# run body, so keeping the entry in its own TU stops it dragging in a second definition
# of kickos_init_entry.
add_library(kickos_default_init STATIC
  init/default_init_entry.cc
  init/default_init_run.cc
  init/console_bringup.cc)
kickos_apply_freestanding(kickos_default_init)
# It must NOT link kickos_core (that would force-include app.h with -Dmain and fire
# app.h's weak-stamp block here). Give it just the include paths it needs: its own
# system/include (init.h) and user/include (app.h).
target_include_directories(kickos_default_init PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/user/include")

# The universal-default console bring-up provider: defines kickos_board_console with
# start = NULL, so the default init keeps the kernel console. Selected by
# KICKOS_CONSOLE_BRINGUP on every board that ships no userspace console driver. Its
# own TU so a board that DOES select a driver provider never also drags this in (one
# definition of kickos_board_console per image).
add_library(kickos_console_none STATIC init/console_none.cc)
kickos_apply_freestanding(kickos_console_none)
target_include_directories(kickos_console_none PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

install(TARGETS kickos_system kickos_default_init kickos_console_none EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
