# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ESP32-C6 CPU-interrupt-controller identity probe: INTPRI @0x600C_5000 read through a
# 256 B PMP window while the kernel writes the undocumented 0x2000_1000 enable. It needs the
# KOS_SYS_IRQ_UNMASK / KOS_SYS_IRQ_INJECT / KOS_SYS_GRANT_PROBE arms, which are compiled in
# under KICKOS_ENABLE_SELFTEST alone.
# No CTest gate: it answers a HARDWARE question and is validated by the operator on silicon.
kickos_add_app_target(c6intpri main.cc)
target_link_libraries(c6intpri PRIVATE kickos)
kickos_emit_image(c6intpri)
