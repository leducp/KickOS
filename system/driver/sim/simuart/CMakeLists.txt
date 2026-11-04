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
                  THREAD service ENTRY uart::console_thread ARG block CAPS ep:wait notify:signal:doorbell)
target_include_directories(kickos_simuart PRIVATE "${PROJECT_BINARY_DIR}/generated/chip")

# The knob of tests/integration/check_sim_drvready.sh: `late`, the IRQ thread sets its readiness
# latch well after the receiver waits on it, or `wedge`, the first instance never sets it.
if(KICKOS_SIMUART_READY STREQUAL "late")
  target_compile_definitions(kickos_simuart PRIVATE KICKOS_SIMUART_READY_LATE=1)
elseif(KICKOS_SIMUART_READY STREQUAL "wedge")
  target_compile_definitions(kickos_simuart PRIVATE KICKOS_SIMUART_READY_WEDGE=1)
elseif(DEFINED KICKOS_SIMUART_READY AND NOT KICKOS_SIMUART_READY STREQUAL "")
  message(FATAL_ERROR "KICKOS_SIMUART_READY is '${KICKOS_SIMUART_READY}', not late or wedge")
endif()
