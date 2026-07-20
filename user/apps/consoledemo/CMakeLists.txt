# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console handover demo (M3 #4 stage ii-a): the kernel relinquishes the XMC4800
# UART to the unprivileged kickos_xmcuart driver and a worker's printf() reaches
# the wire through it. A real user-facing app (kickos_add_application, built on the
# plain xmc4800-relax config), not a diagnostic -- it uses only the production
# endpoint/console-publish ABI. Registered only for the xmc4800-relax board
# (chip-specific USIC window) AND enforcement builds (the handover isolation, and
# the U0C0 grant, are M2 constructs).
if(NOT (KICKOS_BOARD STREQUAL "xmc4800-relax" AND KICKOS_HAVE_MPU))
  return()
endif()

kickos_add_application(consoledemo
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# The polled UART TX transport is the exported kickos_xmcuart driver, not a local copy.
target_link_libraries(consoledemo PRIVATE kickos_xmcuart)

# No CTest gate: this app answers a HARDWARE question (userspace UART ownership +
# the client->driver->wire path) and is validated by the operator on silicon (XMC
# has no QEMU model; a live publish also silences the sim TAP), not by the in-env sim.
