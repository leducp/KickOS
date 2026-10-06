# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The cross-core slay gate riding `slaypeer`: a victim RUNNING on a core other than the
# slayer's, which is the whole hazard.

if(NOT TARGET slaypeer)
  return()
endif()

kickos_add_qemu_test(TARGET slaypeer
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_slaypeer.sh")
kickos_app_judge(slaypeer tests/integration/check_slaypeer.sh)
