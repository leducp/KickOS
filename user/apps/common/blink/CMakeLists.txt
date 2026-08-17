# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Bring-up smoke test: blinks the kernel diagnostic LED via a syscall. Board-agnostic (the
# pin lives in the chip backend, arch_diag_led_*), but built from an ALLOWLIST in
# user/apps/common/CMakeLists.txt: on a chip whose arch_diag_led_set is the no-op fallback
# it would blink nothing and read as a dead board.
kickos_add_application(blink
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
