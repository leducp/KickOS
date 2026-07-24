# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Chip-specific board/device drivers. Each is an exported lib a consumer links on top
# of the OS; they build UNPRIVILEGED but are system board support, not user apps. Only
# the driver directory for THIS build's chip is added.
if(KICKOS_CHIP STREQUAL "mk64f")
  add_subdirectory(mk64f)
elseif(KICKOS_CHIP STREQUAL "xmc4800")
  add_subdirectory(xmc4800)
endif()
