# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F console bring-up demo (M4.3): the DEFAULT INIT relinquishes UART0 to the
# unprivileged kickos_k64uart driver BEFORE this app's main, and a worker's printf()
# reaches the wire through it with zero handover code in the app. A real user-facing app
# (kickos_add_application), not a diagnostic. It uses only the production endpoint/
# console-publish ABI. Registered only for the frdmk64f board (chip-specific UART0
# window + AIPS open) AND enforcement builds (the memory-fault reclaim trigger needs
# SYSMPU; the handover is otherwise coarse-AIPS FUNCTIONAL). On this board the console
# is a KOS_SVC_CONSOLE entry in the default service list
# (KICKOS_SERVICE_LIST=kickos_services_frdmk64f), so the driver is linked through
# kickos_core; the app links no driver lib itself.
if(NOT (KICKOS_BOARD STREQUAL "frdmk64f" AND KICKOS_HAVE_MPU))
  return()
endif()

kickos_add_application(k64console
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# Scramble-then-panic reclaim test mode. OFF by default: this is a deliberate HARDWARE
# test, not the normal handover demo. When ON, k64console spawns an unprivileged
# scrambler (NO MMIO grant) that garbles UART0 through the open AIPS slot to the silent-
# loss state (TXCTSE/baud/TXINV, TX off) and then forces a SYSMPU memory fault, so an
# operator can confirm the "=== HARD FAULT ===" banner STILL reaches the wire
# (arch_console_reclaim recovered the UART). Enable with -DK64CONSOLE_SCRAMBLE_TEST=ON.
# PASS is silicon-observable.
option(K64CONSOLE_SCRAMBLE_TEST "k64console: scramble-then-panic console-reclaim test mode" OFF)
if(K64CONSOLE_SCRAMBLE_TEST)
  target_compile_definitions(k64console PRIVATE K64CONSOLE_SCRAMBLE_TEST=1)
else()
  target_compile_definitions(k64console PRIVATE K64CONSOLE_SCRAMBLE_TEST=0)
endif()

# No CTest gate: this app answers a HARDWARE question (userspace UART ownership +
# the client->driver->wire path + panic-path reclaim) and is validated by the operator
# on K64F silicon, not by the in-env sim (a live publish silences the sim TAP).
