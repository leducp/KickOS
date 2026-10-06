# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos::stdout_write (user/include/kickos/sys/emit.h) against a scripted kernel: the answers
# a send and the kernel console give across a console driver's restart.
kickos_add_unit_test(NAME stdout_write
  SOURCES stdout_write.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_SOURCE_DIR}/lib/include" "${PROJECT_BINARY_DIR}/generated/include"
  KERNEL)
