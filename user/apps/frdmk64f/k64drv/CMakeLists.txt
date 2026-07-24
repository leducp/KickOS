# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F first unprivileged driver over the MMIO-grant seam (task #9 Stage 2): a PIT
# timer driver that exercises unprivileged peripheral MMIO + the tier-1 IRQ path.
# Diagnostic app: built only under KICKOS_ENABLE_SELFTEST (never a production image).
# Registered only for the frdmk64f board (chip-specific PIT/AIPS bring-up).
kickos_add_diagnostic_app(k64drv
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET k64drv)
  return() # diagnostics off -> app not built
endif()

# No CTest gate: this app answers a HARDWARE question (SYSMPU peripheral gating)
# and is validated by the operator on silicon, not by the in-env sim/QEMU.
