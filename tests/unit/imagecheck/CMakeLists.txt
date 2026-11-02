# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The boot check of an image header, one malformed field per check, at both postures: an
# enforcing board's, which checks every span, and a non-enforcing one's, which checks the format
# and the kernel's entry points alone.
set(_ic_sources image_check_test.cc "${PROJECT_SOURCE_DIR}/kernel/task/image_check.cc")
set(_ic_includes "${PROJECT_SOURCE_DIR}/kernel/include" "${PROJECT_SOURCE_DIR}/user/include"
                 "${PROJECT_SOURCE_DIR}/system/include")
kickos_add_unit_test(NAME image_check TEST_PREFIX spans_
  SOURCES ${_ic_sources} INCLUDES ${_ic_includes}
  DEFINITIONS KICKOS_IMAGE_CHECK_SPANS=1)
kickos_add_unit_test(NAME image_check_format TEST_PREFIX format_
  SOURCES ${_ic_sources} INCLUDES ${_ic_includes}
  DEFINITIONS KICKOS_IMAGE_CHECK_SPANS=0)
