# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The drivers of THIS build's chip, each an exported library a consumer links on top of the OS;
# they build UNPRIVILEGED. rp2040 and rp2350 share one directory, the same USB device IP.
set(_chip_drivers "${KICKOS_CHIP}")
if(KICKOS_CHIP STREQUAL "rp2040" OR KICKOS_CHIP STREQUAL "rp2350")
  set(_chip_drivers rp2xxx)
endif()
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_chip_drivers}")
  add_subdirectory(${_chip_drivers})
endif()
