# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ABOVE ONE CORE, which is the whole hazard: at one core a victim cannot be RUNNING anywhere
# but where the slayer is, and thread_cancel_kind hands it its own escalation on the way out.
# THE TARGET ITSELF IS GATED and not just its ctest entry: main.cc pins by core mask and reads
# a printf formatter, neither of which is there below two cores or without a libc.
if(NOT KICKOS_HAVE_LIBC OR NOT KICKOS_BUILD_TESTS OR NOT KICKOS_KERNEL_CORES GREATER 1)
  return()
endif()

kickos_add_application(slaypeer
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET slaypeer)
  return()
endif()

if(KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_slaypeer TARGET slaypeer BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_slaypeer.sh"
    TIMEOUT 120)
elseif(KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_slaypeer TARGET slaypeer BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_slaypeer.sh"
    TIMEOUT 120)
endif()
