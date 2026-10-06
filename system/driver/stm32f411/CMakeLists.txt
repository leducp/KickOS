# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# STM32F411 drivers. The chip is shared by f411disco and blackpill, so nothing here may
# carry a board fact: the window, the lines and the priority all arrive in the instance its
# composition fills.
add_subdirectory(f4uartirq)
