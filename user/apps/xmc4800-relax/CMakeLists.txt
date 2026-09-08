# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# xmc4800-relax-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# xmc4800-relax), and the diagnostic posture is applied here so the app files never ask.

# USIC0-CH1 SSC (SPI) drivers/benches and the console-reclaim test: the MMIO-grant seam and the
# U0C0 grant exist only under enforcement, and conreclaim needs enforcement for the fault to
# happen at all (main.cc #errors otherwise).
if(KICKOS_HAVE_MPU)
  kickos_add_diagnostic_apps(xmcspi xmccshold xmcssc pvprobe inprstorm conreclaim)
endif()

# Console bring-up demo. A real user-facing app, not a diagnostic. Enforcement builds only: the
# isolation and the U0C0 grant exist only there. It self-gates further on the selected service
# list, which must publish a userspace console.
if(KICKOS_HAVE_MPU)
  add_subdirectory(consoledemo)
endif()
