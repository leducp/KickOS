# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# xmc4800-relax-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# xmc4800-relax).

# USIC0 channel 1 SSC drivers and benches over the window each composition grants, and the
# console-reclaim test. Each #errors without enforcement.
if(KICKOS_HAVE_MPU)
  add_subdirectory(xmccshold)
  add_subdirectory(xmcssc)
  add_subdirectory(pvprobe)
  add_subdirectory(inprstorm)
  add_subdirectory(conreclaim)
endif()
