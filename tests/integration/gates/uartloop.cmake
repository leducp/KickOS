# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The buffered-UART loopback arm riding `uartloop`, against the packaged simuart its composition
# names. The loopback "device" is the host wire (see tests/integration/check_sim_uartloop.sh).

if(NOT TARGET uartloop)
  return()
endif()

add_test(NAME sim_uart_loopback
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_uartloop.sh" "$<TARGET_FILE:uartloop>")
set_tests_properties(sim_uart_loopback PROPERTIES TIMEOUT 60)
