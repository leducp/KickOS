# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every image this configure emits answers its libc system calls from KickOS, read from the
# link maps kickos-image-maps.txt lists (the root CMakeLists.txt writes it).

if(NOT KICKOS_ARCH STREQUAL "sim")
  add_test(NAME ${_tag}_images_no_libnosys
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_no_libnosys.sh"
            --images "${PROJECT_BINARY_DIR}/kickos-image-maps.txt")
  kickos_host_gate(${_tag}_images_no_libnosys TIMEOUT 120)
endif()
