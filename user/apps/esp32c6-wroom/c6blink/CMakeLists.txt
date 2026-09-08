# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ESP32-C6 GPIO10 blink over the MMIO-grant seam: the CANONICAL per-thread
# peripheral-MMIO isolation reference on RISC-V PMP, a real GPIO window granted to an
# unprivileged thread (granted blink works, ungranted same-block poke faults mcause=7),
# layered over a one-time TEE-mode APM REE0 background open.
# No CTest gate: it answers a HARDWARE question (C6 PMP plus APM peripheral enforcement) and is
# validated by the operator on silicon.
kickos_add_app_target(c6blink main.cc)
# Driver-model rule 6: the GPIO output-latch readback is shared register logic, so it comes
# from the freestanding kickos_class_esp32c6 leaf.
target_link_libraries(c6blink PRIVATE kickos_class_esp32c6)
target_link_libraries(c6blink PRIVATE kickos)
kickos_emit_image(c6blink)
