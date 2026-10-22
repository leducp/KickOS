# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# user/src/sem.cc over the sim's compare-and-swap, with a scripted kernel behind the word wait and
# a clock the script advances.
kickos_add_unit_test(NAME user_sem
  SOURCES sem_wait.cc "${PROJECT_SOURCE_DIR}/user/src/sem.cc"
          "${PROJECT_SOURCE_DIR}/user/src/arch/sim/user_cas.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include"
  KERNEL)
