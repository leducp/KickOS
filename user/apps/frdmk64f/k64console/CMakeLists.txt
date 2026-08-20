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
# loss state (TXCTSE/baud/TXINV, TX off) and then panics, so an operator can confirm the
# "KERNEL PANIC: [k64console] PASS: the dead channel came back" line STILL reaches the
# wire (arch_console_reclaim recovered the UART out of the PUBLISHED state, with the
# userspace driver still holding it). Enable with -DK64CONSOLE_SCRAMBLE_TEST=ON.
#
# No fault banner is reachable here: fault isolation answers the scrambler's fault by killing
# the thread, and the thread-kill path does not reclaim, so the run goes dark. main.cc records
# why kos_panic is the only terminal path an app has left. This criterion has never been
# witnessed on K64F silicon; take the capture on the next pass.
#
# NOT RETIRED, though conreclaim shows the same reclaim body: that app runs with
# kickos_services_none, so its console is KERNEL_OWNED and never published. This is the only
# arm in the tree that reclaims out of USER_OWNED, the state a real board with a console
# driver actually panics in, and the only one whose garbler reaches the device with no MMIO
# grant at all (coarse AIPS). Deleting it would drop both.
option(K64CONSOLE_SCRAMBLE_TEST "k64console: scramble-then-panic console-reclaim test mode" OFF)
if(K64CONSOLE_SCRAMBLE_TEST)
  target_compile_definitions(k64console PRIVATE K64CONSOLE_SCRAMBLE_TEST=1)
else()
  target_compile_definitions(k64console PRIVATE K64CONSOLE_SCRAMBLE_TEST=0)
endif()

# No CTest gate, and no way to add one: this app answers a HARDWARE question (userspace
# UART ownership + the client->driver->wire path + panic-path reclaim). K64F has no QEMU
# model and the in-env sim has no UART to garble, nor a TAP stream to read once a live
# publish silences it, so the operator on silicon is the only reader either arm can have.
