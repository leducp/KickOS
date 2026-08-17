# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Chip-specific board/device drivers. Each is an exported lib a consumer links on top
# of the OS; they build UNPRIVILEGED but are system board support, not user apps. Only
# the driver directory for THIS build's chip is added.
#
# The EXISTS guards on the chips below are load-bearing while their driver directories are
# being written: a chip whose directory is not there yet must still configure, or every
# other board's build breaks in the meantime. Drop the guard once the directory lands.
if(KICKOS_CHIP STREQUAL "mk64f")
  add_subdirectory(mk64f)
elseif(KICKOS_CHIP STREQUAL "xmc4800")
  add_subdirectory(xmc4800)
elseif(KICKOS_CHIP STREQUAL "rx72m" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rx72m")
  add_subdirectory(rx72m)
elseif(KICKOS_CHIP STREQUAL "esp32c6" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/esp32c6")
  add_subdirectory(esp32c6)
elseif(KICKOS_CHIP STREQUAL "esp32" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/esp32")
  add_subdirectory(esp32)
elseif(KICKOS_CHIP STREQUAL "rp2040" OR KICKOS_CHIP STREQUAL "rp2350")
  # One directory for both parts: the USB device blocks are the same IP, so the driver
  # under it is one body plus a per-chip delta header.
  add_subdirectory(rp2xxx)
elseif(KICKOS_CHIP STREQUAL "imxrt1062" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/imxrt1062")
  add_subdirectory(imxrt1062)
endif()
