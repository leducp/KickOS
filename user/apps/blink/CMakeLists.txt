# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Bring-up smoke test: blinks the kernel diagnostic LED via a syscall. Fully
# board-agnostic -- the pin lives in the chip backend (arch_diag_led_*), so this
# app carries no per-board code. Built for every board (see user/apps/
# CMakeLists.txt); a no-op on any without a known LED.
kickos_add_application(blink
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
