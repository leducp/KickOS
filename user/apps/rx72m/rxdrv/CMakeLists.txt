# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# RX72M GPIO blink over the MMIO-grant seam: the per-thread peripheral-MMIO isolation
# reference on the RX MPU, a real 80 B PORT window (PDR/PODR/PIDR) granted to an unprivileged
# thread (granted LED6 toggle works, ungranted PORT8.PMR poke faults access-exception vector
# 0x54). It deliberately faults, so it is never a production image.
# No CTest gate: it answers a HARDWARE question (RX MPU peripheral enforcement) and is
# validated by the operator on silicon.
kickos_add_app_target(rxdrv main.cc)
# Driver-model rule 6: the PORT output-latch readback is shared register logic, so it comes
# from the freestanding kickos_class_rx72m leaf.
target_link_libraries(rxdrv PRIVATE kickos_class_rx72m)
target_link_libraries(rxdrv PRIVATE kickos)
kickos_emit_image(rxdrv)
