# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# f411disco-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# f411disco).

# The ungranted read must fault, which only an enforcing build can witness.
if(KICKOS_HAVE_MPU)
  add_subdirectory(f411spi)
endif()
