# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console bring-up demo (M4.3): the DEFAULT INIT relinquishes the XMC4800 UART to the
# unprivileged kickos_xmcuart driver BEFORE this app's main, and a plain worker's
# printf() reaches the wire through it with zero handover code in the app. A real
# user-facing app (kickos_add_application, built on the plain xmc4800-relax config),
# not a diagnostic. Registered only for the xmc4800-relax board (chip-specific USIC
# window) AND enforcement builds (the isolation and the U0C0 grant are M2 constructs).
# The driver comes from the selected service list's KOS_SVC_CONSOLE entry through
# kickos_core; the app links no driver lib itself. Without such an entry there is no
# handover to demonstrate and every printf below would take the kernel path, so the
# selected list is a precondition, not a preference.
if(NOT (KICKOS_BOARD STREQUAL "xmc4800-relax" AND KICKOS_HAVE_MPU))
  return()
endif()
set(_consoledemo_lists
  kickos_services_xmc4800relax
  kickos_services_xmc4800relax_console)
if(NOT KICKOS_SERVICE_LIST IN_LIST _consoledemo_lists)
  message(STATUS
    "KickOS: consoledemo skipped: KICKOS_SERVICE_LIST=${KICKOS_SERVICE_LIST} publishes no "
    "userspace console. Build it with -DKICKOS_SERVICE_LIST=kickos_services_xmc4800relax_console.")
  return()
endif()

kickos_add_application(consoledemo
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# No CTest gate: this app answers a HARDWARE question (userspace UART ownership +
# the client->driver->wire path) and is validated by the operator on silicon (XMC
# has no QEMU model; a live publish also silences the sim TAP), not by the in-env sim.
