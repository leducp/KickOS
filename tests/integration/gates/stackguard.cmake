# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `stackguard`: a write into the guard page under a thread stack.

if(NOT TARGET stackguard)
  return()
endif()

# 139 is KOS_EXIT_FAULT and "THREAD FAULT" the thread-kill banner. The gate asserts them as
# literals; computing them from the sources that produce them would assert nothing. The access
# is an unprivileged one, so every translating backend contains it and the marker is the kill
# banner. The arch travels as an argument: the syndrome is spelled per arch.
kickos_add_qemu_test(NAME ${_tag}_stack_guard TARGET stackguard
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stack_guard.sh"
  ARGS "THREAD FAULT" 139 ${KICKOS_ARCH})
