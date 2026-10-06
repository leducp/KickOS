# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# End-to-end buffered-UART exercise against the packaged two-thread simuart its composition
# names. The loopback "device" it drives is the host wire (see
# tests/integration/check_sim_uartloop.sh).
add_executable(uartloop main.cc)
kickos_app_system(uartloop COMPOSITION system.yaml)
kickos_emit_image(uartloop)
