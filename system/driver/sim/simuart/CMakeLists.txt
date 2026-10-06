# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_simuart: the host sim's loopback UART on the buffered-UART substrate, its line the sim
# chip file's, raised by the doorbell alone.
kickos_add_driver(simuart
                  THREADS uartirq:1:default:2:0 service:0:default:2:1 RECEIVER service
                  LINES irq NOTIFY BLOCK ${KICKOS_UART_BLOCK_SIZE}
                  POSTURE retain BARRIER 1 START simuart_start)
target_include_directories(kickos_simuart PRIVATE "${PROJECT_BINARY_DIR}/generated/chip")
