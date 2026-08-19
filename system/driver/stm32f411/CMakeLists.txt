# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# STM32F411 drivers. The chip is shared by f411disco and blackpill, so nothing here may
# carry a board fact: the window base, the rate and the priority all arrive in the
# kos_service_cfg a per-BOARD service-list provider supplies.
add_subdirectory(f4uartirq)
