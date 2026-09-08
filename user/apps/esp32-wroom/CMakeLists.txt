# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# esp32-wroom-only apps. The directory IS the board gate (added only when KICKOS_BOARD is
# esp32-wroom), and the diagnostic posture is applied here so the app files never ask.

# LX6 shared-kernel silicon probe: S32C1I between PRO_CPU and APP_CPU over internal
# SRAM, and PRID on both cores. The APP_CPU launch is chip-specific.
kickos_add_diagnostic_apps(lx6smp)
