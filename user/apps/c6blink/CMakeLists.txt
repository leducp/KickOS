# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ESP32-C6 GPIO10 blink over the MMIO-grant seam: the CANONICAL per-thread
# peripheral-MMIO isolation reference on RISC-V PMP -- a real GPIO window granted to
# an unprivileged thread (granted blink works, ungranted same-block poke faults
# mcause=7), layered over a one-time TEE-mode APM REE0 background open. Diagnostic
# app: built only under KICKOS_ENABLE_SELFTEST (never a production image). Registered
# only for the esp32c6-wroom board (chip-specific GPIO/IO_MUX/APM bring-up).
kickos_add_diagnostic_app(c6blink
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET c6blink)
  return() # diagnostics off -> app not built
endif()

# Rule 6 (M4, decision R-A): the GPIO output-latch readback is shared register
# logic, so it comes from the freestanding kickos_class_esp32c6 leaf -- the same
# object the kernel timebase will link in Phase A -- not a local copy.
target_link_libraries(c6blink PRIVATE kickos_class_esp32c6)

# No CTest gate: this app answers a HARDWARE question (C6 PMP + APM peripheral
# enforcement) and is validated by the operator on silicon, not by the in-env sim/QEMU.
