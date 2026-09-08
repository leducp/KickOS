# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The FP context-switch torture gate riding `fp_switch`: boot the image, assert the checker
# reports the callee-saved bank preserved ("FP OK") and never "FP FAIL".

if(NOT TARGET fp_switch)
  return()
endif()

# One call for every board that builds the app, the enclosing add_subdirectory gate deciding
# which those are (user/apps/common/CMakeLists.txt). The M-profile arm validates the PendSV FP
# save/restore over s16-s31 on real armv7m before flashing; the arm64 arm is the same torture
# over ALL THIRTY-TWO vector registers, A64 auto-stacking no FP state at all, so an
# asynchronous entry there owes the whole bank and not just the callee-saved half. One script
# covers both, and each board reports under the derived <board tag>_fp_switch name.
kickos_add_qemu_test(TARGET fp_switch
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_fp.sh")
