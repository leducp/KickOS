# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_cxx in place of kickos flips the app's C++ TUs to -fexceptions/-frtti and links the
# toolchain libstdc++/libsupc++ over newlib. It is the one app in the tree that opts in; the
# board list that builds it is in user/apps/common/CMakeLists.txt.
kickos_add_app_target(cxxtest main.cc)
target_link_libraries(cxxtest PRIVATE kickos_cxx)
kickos_emit_image(cxxtest)
