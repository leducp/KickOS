# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# RX72M LED6 blink over the port window its composition grants, then a child thread's write to the
# ungranted MPC, which the RX MPU must refuse. Judged from a silicon capture.
add_executable(rxdrv main.cc)
# The PORT output-latch readback is shared register logic, from the freestanding class leaf.
target_link_libraries(rxdrv PRIVATE kickos_class_rx72m)
kickos_app_system(rxdrv COMPOSITION system.yaml)
kickos_emit_image(rxdrv)
kickos_app_judge(rxdrv tests/integration/check_rxdrv.sh)
