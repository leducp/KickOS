# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# TRANSLATING AND ABOVE ONE CORE, both load-bearing: the counter it reads only exists where a
# space can be installed on a core at all, and a peer core is the whole hazard. The probe is
# selftest scaffolding, so the selftest posture is required too. THE TARGET ITSELF IS GATED and
# not just its ctest entry: main.cc calls kos_aspace_probe unconditionally, which is not
# declared where KICKOS_HAVE_ASPACE is 0.
if(NOT KICKOS_HAVE_LIBC OR NOT KICKOS_BUILD_TESTS OR NOT KICKOS_HAVE_ASPACE
   OR NOT KICKOS_KERNEL_CORES GREATER 1 OR NOT KICKOS_ENABLE_SELFTEST)
  return()
endif()

kickos_add_application(taskleave
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET taskleave)
  return()
endif()

if(KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_taskleave TARGET taskleave BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_taskleave.sh"
    TIMEOUT 120)
elseif(KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_taskleave TARGET taskleave BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_taskleave.sh"
    TIMEOUT 120)
endif()
