# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `deadstack`: a page withdrawn under a reader holding its translation.

if(NOT TARGET deadstack)
  return()
endif()

# 139 is KOS_EXIT_FAULT and "THREAD FAULT" the thread-kill banner. The gate asserts them as
# literals; computing them from the sources that produce them would assert nothing. The read is
# an unprivileged one, so fault isolation contains it and the record is the kill banner. The
# arch travels as an argument: the syndrome is spelled per arch.
kickos_add_qemu_test(TARGET deadstack
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_deadstack.sh"
  ARGS "THREAD FAULT" 139 ${KICKOS_ARCH})
