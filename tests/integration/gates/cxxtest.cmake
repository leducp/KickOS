# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The full-C++ smoke gate riding `cxxtest`.

if(NOT TARGET cxxtest)
  return()
endif()

# One call for every board: the boards that build this app at all are enumerated in
# user/apps/common/CMakeLists.txt, and every emulatable one of them boots the image through
# this same script and reports under the derived <board tag>_cxxtest name.
kickos_add_qemu_test(TARGET cxxtest
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_cxxtest.sh")
