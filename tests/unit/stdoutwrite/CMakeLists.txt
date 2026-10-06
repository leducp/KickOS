# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos::stdout_write (user/include/kickos/sys/emit.h), and kos_print and libc's _write over it,
# against a scripted kernel: the answers a send and the kernel console give across a console
# driver's restart.
kickos_add_unit_test(NAME stdout_write
  SOURCES stdout_write.cc "${PROJECT_SOURCE_DIR}/user/src/print.cc"
          "${PROJECT_SOURCE_DIR}/tests/unit/newlibstubs/newlib_stubs_shim.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_SOURCE_DIR}/lib/include" "${PROJECT_BINARY_DIR}/generated/include"
  KERNEL)

# user/src/newlib_fcntl.cc against a scripted kos_task_nonblock, renamed off the host libc's own.
kickos_add_unit_test(NAME fcntl_flags
  SOURCES fcntl_flags.cc "${PROJECT_SOURCE_DIR}/user/src/newlib_fcntl.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_SOURCE_DIR}/lib/include" "${PROJECT_BINARY_DIR}/generated/include"
  DEFINITIONS fcntl=kickos_test_fcntl _fcntl=kickos_test__fcntl
  KERNEL)
