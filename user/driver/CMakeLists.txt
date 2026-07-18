# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Chip-specific unprivileged userspace drivers, each an exported lib a consumer
# links on top of the OS. Only the driver for THIS build's chip is defined.
if(KICKOS_CHIP STREQUAL "mk64f")
  add_subdirectory(k64dspi)
endif()
