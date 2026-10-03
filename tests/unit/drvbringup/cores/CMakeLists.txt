# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos::driver::bring_up built for more than one kernel core, where it pins itself for its claims
# and places each thread, over the recording seam of the directory above.
#
# HOST-ONLY on the same mechanism as drv_bringup: kos_seam.cc defines public kos_* names.
kickos_add_unit_test(NAME drv_bringup_cores
  SOURCES bringup_cores.cc "${CMAKE_CURRENT_SOURCE_DIR}/../kos_seam.cc"
          "${PROJECT_SOURCE_DIR}/user/src/driver_service.cc"
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/.." "${PROJECT_SOURCE_DIR}/user/include"
           "${PROJECT_SOURCE_DIR}/system/include" "${PROJECT_BINARY_DIR}/generated/include"
  POSTURE KICKOS_KERNEL_CORES=2)
