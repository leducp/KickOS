# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# RX72M IRQ15 on P07, a plain INTB vector, claimed and driven from the pin's own output latch.
# Judged from a silicon capture.
add_executable(rxirq main.cc)
kickos_app_system(rxirq COMPOSITION system.yaml)
kickos_emit_image(rxirq)
kickos_app_judge(rxirq tests/integration/check_rxirq.sh)
