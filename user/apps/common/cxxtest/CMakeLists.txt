# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Full-C++ opt-in smoke test (Stage B of docs/design-kickcat-k64f.md). FULL_CXX
# flips the app's C++ TUs to -fexceptions/-frtti and links the toolchain
# libstdc++/libsupc++ over newlib -- the one app in the tree that opts in; every
# other app stays freestanding. Gated to the boards where it is exercised (the
# runnable QEMU armv7m + the K64F silicon target) in user/apps/CMakeLists.txt.
kickos_add_application(cxxtest
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD}
  FULL_CXX)

# QEMU armv7m gate: boot the image on a QEMU Cortex-M4 (mps2-an386) and assert the
# exceptions/STL/RTTI checks all pass -- end-to-end proof that libstdc++ executes
# on real armv7m without hardware. The app returns 0 -> semihosting SYS_EXIT.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  kickos_add_qemu_test(NAME qemu_cxxtest TARGET cxxtest BOARD qemu
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_cxxtest.sh")
endif()

# QEMU RISC-V gate: the same full-C++ smoke on a QEMU `virt` (RV32IMAC, M-mode),
# proving the RISCStar newlib libstdc++ over the same porting-layer seam.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_cxxtest TARGET cxxtest BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_cxxtest.sh")
endif()
