# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# PRIVILEGED runtime clock-retune silicon harness (M3 clock-select coherence tail).
# A deliberate HARDWARE test, OFF by default: it drives a real privileged retune
# (mask/disarm/flush/re-anchor/baud/re-arm) that no emulator exercises, so PASS is
# silicon-observable on the operator's console. Enable with
# -DKICKOS_CLOCK_RETUNE_TEST=ON. Only the two backends that strong-override
# arch_cpu_clock_set (XMC4800, K64F) under enforcement -- elsewhere the retune is a
# weak-default no-op and this harness would prove nothing.
option(KICKOS_CLOCK_RETUNE_TEST "clockretune: privileged runtime clock-retune HW harness" OFF)
if(NOT KICKOS_CLOCK_RETUNE_TEST)
  return()
endif()
if(NOT ((KICKOS_BOARD STREQUAL "xmc4800-relax" OR KICKOS_BOARD STREQUAL "frdmk64f")
        AND KICKOS_HAVE_MPU))
  return()
endif()

kickos_add_application(clockretune
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# No CTest gate: this app answers a HARDWARE question (does the privileged retune
# coherence tail hold on real silicon -- flash-WS/voltage ordering, monotonic now,
# baud re-derivation) validated by the operator on the wire, not by the in-env sim.
