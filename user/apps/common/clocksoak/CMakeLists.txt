# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Clock-hardening SILICON soak harness. It idles past one counter wrap period and across
# several wraps, then reports monotonicity and rate on the operator's console: the
# "silicon-test-later" checklist no emulator exercises, a wide free-running peripheral counter
# behaving the same in QEMU regardless of the wrap/WFI hazard. Arch-neutral (clock_now plus
# sleep_ns only), so it builds for any board.
# No CTest gate: it answers a HARDWARE question (does the wide-counter monotonic clock survive
# real wraps and idle) validated by the operator on the wire.
kickos_add_app_target(clocksoak main.cc)
target_link_libraries(clocksoak PRIVATE kickos)
kickos_emit_image(clocksoak)

set(KICKOS_CLOCKSOAK_WRAP_MS 60000 CACHE STRING "clocksoak: counter wrap period in ms")
set(KICKOS_CLOCKSOAK_WRAPS 3 CACHE STRING "clocksoak: number of wraps to soak across")
target_compile_definitions(clocksoak PRIVATE
  KICKOS_CLOCKSOAK_WRAP_MS=${KICKOS_CLOCKSOAK_WRAP_MS}u
  KICKOS_CLOCKSOAK_WRAPS=${KICKOS_CLOCKSOAK_WRAPS}u)
