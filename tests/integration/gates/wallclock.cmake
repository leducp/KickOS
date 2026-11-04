# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# wallclock on every emulated board: the sleeps on the kernel clock, the wall-clock set its
# composition admits and the steady clock across it. The capture host's clock is a capture's.

if(NOT TARGET wallclock)
  return()
endif()
kickos_add_qemu_test(TARGET wallclock SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_wallclock.sh"
  ARGS "${PROJECT_SOURCE_DIR}")
