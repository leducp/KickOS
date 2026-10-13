# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The C library's lock riding `libcrace`, on every emulated board.

if(NOT TARGET libcrace)
  return()
endif()
if(KICKOS_ARCH STREQUAL "sim")
  string(CONCAT _libcrace_why "the sim runs an instance's threads on one host thread, which "
    "the host libc's stdio lock cannot separate")
  kickos_inapplicable(libcrace "${_libcrace_why}")
  return()
endif()
kickos_app_judge(libcrace tests/integration/check_libcrace.sh)
kickos_add_qemu_test(TARGET libcrace
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libcrace.sh")
