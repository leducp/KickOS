# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ROOT-confinement gate, in its own binary (it ends the process on the deliberate
# cross-domain fault when root is confined). Diagnostic app: built only under
# KICKOS_ENABLE_SELFTEST. Board-agnostic (pure syscall plus kos_ram_alloc).
kickos_add_diagnostic_app(rootfault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET rootfault)
  return() # diagnostics off -> app not built; skip its tests
endif()

# Registered ONLY in the flipped posture: the app runs in both and asserts a
# different outcome in each (see main.cc), but the fleet emulator gates all build
# KICKOS_ROOT_PRIVILEGED=ON, where the privileged-root arm is a hand-run control.
# Delete the condition once a flipped board is a standing gate (TODO.md stage 2).
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND NOT KICKOS_ROOT_PRIVILEGED)
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME rootfault COMMAND "$<TARGET_FILE:rootfault>")
    set_tests_properties(rootfault PROPERTIES
      TIMEOUT 15
      PASS_REGULAR_EXPRESSION "MPU FAULT: task 'root'"
      FAIL_REGULAR_EXPRESSION "ERROR|NOT confined")
  elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_rootfault TARGET rootfault
      BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_rootfault.sh")
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv")
    kickos_add_qemu_test(NAME qemu_riscv_rootfault TARGET rootfault BOARD qemu-riscv
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_rootfault.sh")
  endif()
endif()
