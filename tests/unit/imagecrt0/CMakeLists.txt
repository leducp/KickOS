# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# user/src/image_start.cc's kickos_image_crt0 over a header the arm states, where each declared
# task runs an image of its own and no backend translates (the sim's posture).
kickos_add_unit_test(NAME image_crt0
  SOURCES image_crt0_test.cc "${PROJECT_SOURCE_DIR}/user/src/image_start.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_SOURCE_DIR}/lib/include" "${PROJECT_BINARY_DIR}/generated/include"
  KERNEL)
