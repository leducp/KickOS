# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# rx72m-only apps. The directory IS the board gate (added only when KICKOS_BOARD is rx72m).

# The ungranted write must fault, which only an enforcing build can witness.
if(KICKOS_HAVE_MPU)
  add_subdirectory(rxdrv)
endif()
