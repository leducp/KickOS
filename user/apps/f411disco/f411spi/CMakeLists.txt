# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# STM32F411 SPI1 loopback driver over the MMIO-grant seam: the CANONICAL per-thread
# peripheral-MMIO isolation reference on ARMv7-M PMSA, a real SPI1 window granted to an
# unprivileged thread, proven both ways (granted works, ungranted faults MemManage).
# No CTest gate: it answers a HARDWARE question (PMSA peripheral enforcement) and is validated
# by the operator on silicon, not by the in-env sim/QEMU.
kickos_add_app_target(f411spi main.cc)
target_link_libraries(f411spi PRIVATE kickos)
kickos_emit_image(f411spi)
