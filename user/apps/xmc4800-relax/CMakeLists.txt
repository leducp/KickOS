# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# xmc4800-relax-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# xmc4800-relax), and the diagnostic posture is applied here so the app files never ask.

# USIC0 channel 1 SSC drivers and benches over the window each composition grants, and the
# console-reclaim test. Each #errors without enforcement.
if(KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(xmcspi xmccshold xmcssc pvprobe inprstorm conreclaim)
endif()

# Console bring-up demo: a real user-facing app, not a diagnostic. Its composition names the
# packaged xmcuart as stdout; enforcement builds only.
if(KICKOS_HAVE_MPU)
  add_subdirectory(consoledemo)
endif()
