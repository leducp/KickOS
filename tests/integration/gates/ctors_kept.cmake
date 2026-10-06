# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No image this configure emits discards a constructor or destructor table, read from the link
# maps kickos-image-maps.txt lists (the root CMakeLists.txt writes it).

if(NOT KICKOS_ARCH STREQUAL "sim")
  add_test(NAME ${_tag}_images_ctors_kept
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_ctors_kept.sh"
            --images "${PROJECT_BINARY_DIR}/kickos-image-maps.txt")
  kickos_host_gate(${_tag}_images_ctors_kept TIMEOUT 120)
endif()
