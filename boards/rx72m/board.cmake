# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor for a Renesas RX72M board (RX72M Group, RXv3 core). Included
# by cmake/toolchain-rx-elf.cmake. Sets the arch/family/chip resolution and the
# per-board CPU baseline in ONE place (the spike sec.1 board-descriptor seam).
#
#   KICKOS_ARCH_FAMILY  rx   -> arch/rx/... source tree + include routing
#   KICKOS_ARCH         rxv3 -> arch/rx/rxv3 backend (context switch, syscall)
#   KICKOS_CHIP         rx72m-> arch/rx/chip/rx72m (startup, clocks, console)

set(KICKOS_BOARD_ID   "rx72m")
set(KICKOS_ARCH_FAMILY "rx")
set(KICKOS_ARCH        "rxv3")
set(KICKOS_CHIP        "rx72m")

# -misa=v3 selects the RXv3 instruction set (defines __RXv3__); this GNU RX build
# has no -mcpu=rxv3 (its -mcpu= takes device names, e.g. rx72t, also RXv3). The
# ISA selector is the device-independent, correct choice. -mdfpu enables the RX72M
# double-precision FPU + 64-bit doubles, so `double` is 64-bit here as on the
# other boards (not the GNU RX default 32-bit); the SWINT context switch banks the
# DPFPU register file (DR0-DR15 + DPSW/DCMR/DECNT) so it is switch-safe (switch.S).
# KICKOS_MCPU is the uniform descriptor field every board sets (the rx toolchain
# reads it into _kos_cpu, like the arm toolchain).
set(KICKOS_MCPU -misa=v3 -mdfpu)
