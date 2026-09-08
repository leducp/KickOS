# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F first unprivileged driver over the MMIO-grant seam: a PIT timer driver that exercises
# unprivileged peripheral MMIO plus the tier-1 IRQ path.
# No CTest gate: it answers a HARDWARE question (SYSMPU peripheral gating) and is validated by
# the operator on silicon, not by the in-env sim/QEMU.
kickos_add_app_target(k64drv main.cc)
target_link_libraries(k64drv PRIVATE kickos)
kickos_emit_image(k64drv)
