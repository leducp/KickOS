# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# End-to-end buffered-UART exercise against the real two-thread driver. The loopback
# "device" it drives is the host wire (see tests/integration/check_sim_uartloop.sh).

# BUILD only in the posture that provides the service. The app calls
# kickos_sim_uart_take_endpoint, which kickos_services_simuart defines, so linking it
# against any other service list is an undefined reference; fail-loud by construction,
# and the reason this is a build condition rather than a weak fallback (no KickOS seam is
# a weak symbol).
if(KICKOS_SERVICE_LIST STREQUAL "kickos_services_simuart")
  kickos_add_app_target(uartloop main.cc)
  target_link_libraries(uartloop PRIVATE kickos)
  kickos_emit_image(uartloop)
endif()
