# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The buffered-UART loopback arm. It rides `uartloop`, which THIS tree does not build: the
# app links against kickos_services_simuart and the script configures that tree itself, so
# there is no target here whose existence could stand for the gate's precondition. The
# loopback "device" is the host wire (see tests/integration/check_sim_uartloop.sh).

if(NOT KICKOS_ARCH STREQUAL "sim")
  return()
endif()

# REGISTER only in the ordinary tree: the gate configures its own tree with the simuart
# list, so registering there would recurse. The condition mirrors the app's own, because the
# script needs the binary to exist.
if(KICKOS_ENABLE_SELFTEST AND KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  add_test(
    NAME    sim_uart_loopback
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_uartloop.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_uart_loopback PROPERTIES TIMEOUT 300)
endif()
