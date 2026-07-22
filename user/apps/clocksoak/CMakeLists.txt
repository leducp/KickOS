# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Clock-hardening SILICON soak harness. OFF by default; enable with
# -DKICKOS_CLOCKSOAK_TEST=ON. It idles past >1 counter wrap period and across
# several wraps, then reports monotonicity + rate on the operator's console -- the
# "silicon-test-later" checklist that no emulator exercises (a wide free-running
# peripheral counter behaves the same in QEMU regardless of the wrap/WFI hazard).
# Arch-neutral (clock_now + sleep_ns only), so it builds for any board; the per-board
# wrap period is passed with -DKICKOS_CLOCKSOAK_WRAP_MS (default 60000).
option(KICKOS_CLOCKSOAK_TEST "clocksoak: clock-hardening wrap/soak HW harness" OFF)
if(NOT KICKOS_CLOCKSOAK_TEST)
  return()
endif()

set(KICKOS_CLOCKSOAK_WRAP_MS 60000 CACHE STRING "clocksoak: counter wrap period in ms")
set(KICKOS_CLOCKSOAK_WRAPS 3 CACHE STRING "clocksoak: number of wraps to soak across")

kickos_add_application(clocksoak
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

target_compile_definitions(clocksoak PRIVATE
  KICKOS_CLOCKSOAK_WRAP_MS=${KICKOS_CLOCKSOAK_WRAP_MS}u
  KICKOS_CLOCKSOAK_WRAPS=${KICKOS_CLOCKSOAK_WRAPS}u)

# No CTest gate: this answers a HARDWARE question (does the wide-counter monotonic
# clock survive real wraps and idle) validated by the operator on the wire.
