# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A throw, RTTI and libstdc++ over newlib, in the C++ KickOS::kernel compiles an app with.
add_executable(cxxtest main.cc)
target_link_libraries(cxxtest PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(cxxtest)
