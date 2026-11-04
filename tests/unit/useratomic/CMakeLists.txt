# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# user/src/user_atomic.cc over the sim's compare-and-swap, which the arm wraps to land a peer's
# store between a loop's load and its compare.
kickos_add_unit_test(NAME user_atomic
  SOURCES user_atomic_test.cc "${PROJECT_SOURCE_DIR}/user/src/user_atomic.cc"
          "${PROJECT_SOURCE_DIR}/user/src/arch/sim/user_cas.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include")
target_link_options(user_atomic PRIVATE -Wl,--wrap=kickos_user_cas)
