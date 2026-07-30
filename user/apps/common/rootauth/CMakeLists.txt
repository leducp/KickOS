# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ROOT-narrow gate. Diagnostic app: built only under KICKOS_ENABLE_SELFTEST.
# Board-agnostic: pure syscall, no MMIO. One 16-byte kos_ram_alloc (the AUTH_MEMORY arm).
kickos_add_diagnostic_app(rootauth
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET rootauth)
  return() # diagnostics off -> app not built; skip its tests
endif()

# Registered in BOTH postures: the discriminating arms are #if-selected on
# KICKOS_ROOT_PRIVILEGED (see main.cc), so each posture asserts its own outcome. No
# KICKOS_HAVE_MPU condition either, authority being a capability check rather than memory
# enforcement.
if(KICKOS_BUILD_TESTS)
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME rootauth COMMAND "$<TARGET_FILE:rootauth>")
    # Anchored on the app's own prefix, like check_qemu_rootauth.sh: a bare ERROR|FAIL
    # matches those substrings anywhere in the boot output.
    # The panic clause closes a post-verdict hole: main prints PASS and then returns, and
    # the return calls kos_shutdown (gated on KOS_AUTH_SYSTEM), so a regression dropping
    # only that bit panics AFTER the PASS regex has matched.
    set_tests_properties(rootauth PROPERTIES
      TIMEOUT 15
      PASS_REGULAR_EXPRESSION "\\[rootauth\\] PASS"
      FAIL_REGULAR_EXPRESSION "\\[rootauth\\] (ERROR|FAIL);unreachable: root: shutdown refused")
  elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_rootauth TARGET rootauth
      BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_rootauth.sh")
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv")
    kickos_add_qemu_test(NAME qemu_riscv_rootauth TARGET rootauth BOARD qemu-riscv
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_rootauth.sh")
  endif()
endif()
