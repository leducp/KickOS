# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No image this configure emits reads its thread pointer on the boot path, read from the images
# kickos-image-maps.txt lists (the root CMakeLists.txt writes it). Registered on the arches that
# seat the register at switch-in; it runs no image, so it carries the host label.

if(KICKOS_ARCH MATCHES "^(lx6|rv32imac|rv64imac|armv8a)$")
  add_test(NAME ${_tag}_images_boot_tp
    COMMAND python3 "${PROJECT_SOURCE_DIR}/tests/static/check_boot_tp.py"
            "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}"
            --images "${PROJECT_BINARY_DIR}/kickos-image-maps.txt")
  kickos_host_gate(${_tag}_images_boot_tp TIMEOUT 300)
endif()
