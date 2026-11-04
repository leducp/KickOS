# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# wallwait on every emulated board that builds it: system_clock waits across a wall-clock set.

if(NOT TARGET wallwait)
  return()
endif()
kickos_add_qemu_test(TARGET wallwait
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_wallwait.sh")
