# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console bring-up demo (M4.3): the DEFAULT INIT relinquishes the XMC4800 UART to the
# unprivileged kickos_xmcuart driver BEFORE this app's main, and a plain worker's
# printf() reaches the wire through it with zero handover code in the app. A real
# user-facing app (kickos_add_application, built on the plain xmc4800-relax config),
# not a diagnostic. Registered only for the xmc4800-relax board (chip-specific USIC
# window) AND enforcement builds (the isolation and the U0C0 grant are M2 constructs).
# On this board the default KICKOS_CONSOLE_BRINGUP is kickos_console_xmcuart, so the
# driver is linked through kickos_core; the app links no driver lib itself.
if(NOT (KICKOS_BOARD STREQUAL "xmc4800-relax" AND KICKOS_HAVE_MPU))
  return()
endif()

kickos_add_application(consoledemo
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# Scramble-then-panic reclaim test mode (design D-test 3). OFF by default: this is a
# deliberate HARDWARE test, not the normal handover demo. When ON, consoledemo spawns
# an unprivileged scrambler that garbles the granted U0C0 window to the true silent-
# loss state (KSCFG.MODEN=0, gated clock) and then forces an MPU fault, so an operator
# can confirm the "KERNEL PANIC" banner STILL reaches the wire (arch_console_reclaim
# recovered the UART). Enable with -DKICKOS_SCRAMBLE_TEST=ON. PASS is silicon-observable.
option(KICKOS_SCRAMBLE_TEST "consoledemo: scramble-then-panic console-reclaim test mode" OFF)
if(KICKOS_SCRAMBLE_TEST)
  target_compile_definitions(consoledemo PRIVATE CONSOLEDEMO_SCRAMBLE_TEST=1)
else()
  target_compile_definitions(consoledemo PRIVATE CONSOLEDEMO_SCRAMBLE_TEST=0)
endif()

# No CTest gate: this app answers a HARDWARE question (userspace UART ownership +
# the client->driver->wire path) and is validated by the operator on silicon (XMC
# has no QEMU model; a live publish also silences the sim TAP), not by the in-env sim.
