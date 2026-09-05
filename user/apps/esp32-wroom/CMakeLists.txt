# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# esp32-wroom-only apps. The directory is the board gate (added only when
# KICKOS_BOARD is esp32-wroom); each app keeps its own selftest gate.

# LX6 shared-kernel silicon probe: S32C1I between PRO_CPU and APP_CPU over internal
# SRAM, and PRID on both cores. Self-gates on KICKOS_ENABLE_SELFTEST.
add_subdirectory(lx6smp)
