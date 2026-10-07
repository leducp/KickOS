# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `specfault`: a read of an address nothing grants the thread is denied.

if(NOT TARGET specfault)
  return()
endif()
set(_specfault_script "${PROJECT_SOURCE_DIR}/tests/integration/check_specfault.sh")
kickos_app_judge(specfault tests/integration/check_specfault.sh)

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME ${_tag}_specfault COMMAND "${_specfault_script}" "$<TARGET_FILE:specfault>")
  kickos_boot_timeout(${_tag}_specfault "${_specfault_script}")
endif()
kickos_add_qemu_test(TARGET specfault SCRIPT "${_specfault_script}")
