# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# RX72M GPIO blink over the MMIO-grant seam: the per-thread peripheral-MMIO
# isolation reference on the RX MPU -- a real PODR window granted to an unprivileged
# thread (granted LED6 toggle works, ungranted PDR poke faults access-exception
# vector 0x54). Diagnostic app: built only under KICKOS_ENABLE_SELFTEST (it
# deliberately faults; never a production image). Registered only for the rx72m board
# (chip-specific PORT8 bring-up) and enforcement builds.
kickos_add_diagnostic_app(rxdrv
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET rxdrv)
  return() # diagnostics off -> app not built
endif()

# No CTest gate: this app answers a HARDWARE question (RX MPU peripheral enforcement)
# and is validated by the operator on silicon, not by the in-env sim/QEMU.
