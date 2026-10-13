# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The FP context-switch torture gate riding `fp_switch`: boot the image, assert the checker
# reports its FP state preserved ("FP OK") and never "FP FAIL".

if(NOT TARGET fp_switch)
  return()
endif()
kickos_app_judge(fp_switch tests/integration/check_qemu_fp.sh)

# One call for every board that builds the app, the enclosing add_subdirectory gate deciding
# which those are (user/apps/common/CMakeLists.txt). Each board reports under the derived
# <board tag>_fp_switch name.
kickos_add_qemu_test(TARGET fp_switch
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_fp.sh")
