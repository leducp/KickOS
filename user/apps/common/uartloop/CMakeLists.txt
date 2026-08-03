# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# End-to-end buffered-UART exercise against the real two-thread driver. Diagnostic app
# (KICKOS_ENABLE_SELFTEST only); sim-only, because the loopback "device" it drives is the
# host wire (see tests/check_sim_uartloop.sh).
if(NOT KICKOS_ARCH STREQUAL "sim")
  return()
endif()

# BUILD only in the posture that provides the service. The app calls
# kickos_sim_uart_take_endpoint, which kickos_services_simuart defines, so linking it
# against any other service list is an undefined reference -- fail-loud by construction,
# and the reason this is a build condition rather than a weak fallback (no KickOS seam is
# a weak symbol).
if(KICKOS_SERVICE_LIST STREQUAL "kickos_services_simuart")
  kickos_add_diagnostic_app(uartloop
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
endif()

# REGISTER only in the ordinary tree: the gate configures its own tree with the simuart
# list, so registering there would recurse. The condition mirrors
# kickos_add_diagnostic_app's, because the script needs the binary to exist.
if(KICKOS_BUILD_TESTS AND KICKOS_ENABLE_SELFTEST
   AND KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  add_test(
    NAME    sim_uart_loopback
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_sim_uartloop.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_uart_loopback PROPERTIES TIMEOUT 300)
endif()
