# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A throw, RTTI and libstdc++ over newlib, in the C++ KickOS::kernel compiles an app with. Where
# each task has a space, its peer runs the same checks in a space of its own.
add_executable(cxxtest main.cc threads.cc)
target_include_directories(cxxtest PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
if(KICKOS_HAVE_ASPACE)
  add_executable(cxxtest_peer peer.cc)
  target_link_libraries(cxxtest_peer PRIVATE kickos)
  kickos_app_system(cxxtest COMPOSITION system.yaml)
else()
  target_link_libraries(cxxtest PRIVATE KickOS::kickos KickOS::system_default)
endif()
kickos_emit_image(cxxtest)
