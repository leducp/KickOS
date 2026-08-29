# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# FULL_CXX flips the app's C++ TUs to -fexceptions/-frtti and links the toolchain
# libstdc++/libsupc++ over newlib. It is the one app in the tree that opts in; the board list
# that builds it is in user/apps/common/CMakeLists.txt.
kickos_add_application(cxxtest
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD}
  FULL_CXX)

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_cxxtest TARGET cxxtest BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_cxxtest.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_cxxtest TARGET cxxtest BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_cxxtest.sh")
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_cxxtest TARGET cxxtest BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_cxxtest.sh")
endif()
