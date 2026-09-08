# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# PRIVILEGED runtime clock-retune silicon harness (the clock-select coherence tail). It drives
# a real privileged retune (mask/disarm/flush/re-anchor/baud/re-arm) that no emulator
# exercises, so PASS is silicon-observable on the operator's console.
# No CTest gate: it answers a HARDWARE question (does the privileged retune coherence tail
# hold on real silicon: flash-WS/voltage ordering, monotonic now, baud re-derivation)
# validated by the operator on the wire, not by the in-env sim.
kickos_add_app_target(clockretune main.cc)
target_link_libraries(clockretune PRIVATE kickos)
kickos_emit_image(clockretune)
