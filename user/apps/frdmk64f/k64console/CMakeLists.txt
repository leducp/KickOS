# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F console bring-up demo: the DEFAULT INIT relinquishes UART0 to the unprivileged
# kickos_k64uart driver BEFORE this app's main, and a worker's printf() reaches the wire
# through it with zero handover code in the app. A real user-facing app, not a diagnostic; it
# uses only the production endpoint/console-publish ABI. On this board the console is a
# KOS_SVC_CONSOLE entry in the default service list, so the driver is linked through
# kickos_core and the app links no driver lib itself.
#
# An OPERATOR on silicon is the reader of both arms: this app answers a HARDWARE question
# (userspace UART ownership, the client->driver->wire path, and panic-path reclaim), and a live
# publish silences the TAP stream.
kickos_add_app_target(k64console main.cc)
target_link_libraries(k64console PRIVATE kickos)
kickos_emit_image(k64console)

# Scramble-then-panic reclaim test mode. OFF by default: this is a deliberate HARDWARE
# test, not the normal handover demo. When ON, k64console spawns an unprivileged
# scrambler (NO MMIO grant) that garbles UART0 through the open AIPS slot to the silent-
# loss state (TXCTSE/baud/TXINV, TX off) and then panics, so an operator can confirm the
# "KERNEL PANIC: [k64console] PASS: the dead channel came back" line STILL reaches the
# wire (arch_console_reclaim recovered the UART out of the PUBLISHED state, with the
# userspace driver still holding it).
#
# kos_panic is the only terminal path that reclaims: fault isolation answers the scrambler's
# fault by killing the thread, and the thread-kill path does not reclaim (main.cc records
# why). This criterion has never been witnessed on K64F silicon; take the capture on the next
# pass.
#
# The only arm in the tree that reclaims out of USER_OWNED, the state a real board with a
# console driver panics in, and the only one whose garbler reaches the device with no MMIO
# grant at all (coarse AIPS). conreclaim's console is KERNEL_OWNED and never published.
#
# Always defined, never merely absent: the app is built -Wundef -Werror.
option(K64CONSOLE_SCRAMBLE_TEST "k64console: scramble-then-panic console-reclaim test mode" OFF)
if(K64CONSOLE_SCRAMBLE_TEST)
  target_compile_definitions(k64console PRIVATE K64CONSOLE_SCRAMBLE_TEST=1)
else()
  target_compile_definitions(k64console PRIVATE K64CONSOLE_SCRAMBLE_TEST=0)
endif()
