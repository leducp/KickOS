# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_simuart: the host sim's loopback UART on the buffered-UART substrate, its line the sim
# chip file's, raised by the doorbell alone.
#
# EDGE, as the relay rule requires of a line whose waiter holds no window.
kickos_add_driver(simuart
                  LINES irq:edge BLOCK ${KICKOS_UART_BLOCK_SIZE} INIT block_init
                  BARRIER 1 READY uart::KOS_UART_READY_OFFSET POSTURE retain START simuart_start
                  THREAD uartirq PRIORITY 1 ENTRY uart_irq_thread ARG block CAPS notify:wait irq:wait
                  THREAD service ENTRY uart_service_thread ARG block CAPS ep:wait notify:signal:doorbell)
target_include_directories(kickos_simuart PRIVATE "${PROJECT_BINARY_DIR}/generated/chip")
