# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The FP state an FP-live caller gets back from a register call the SVC fastpath refused.

if(NOT TARGET svcfp)
  return()
endif()
kickos_app_judge(svcfp tests/integration/check_svcfp.sh)
kickos_add_qemu_test(TARGET svcfp SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_svcfp.sh")
