# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console bring-up demo: the DEFAULT INIT relinquishes the XMC4800 UART to the unprivileged
# kickos_xmcuart driver BEFORE this app's main, and a plain worker's printf() reaches the wire
# through it with zero handover code in the app. A real user-facing app, not a diagnostic. The
# driver comes from the selected service list's KOS_SVC_CONSOLE entry through kickos_core; the
# app links no driver lib itself.
# No CTest gate: it answers a HARDWARE question (userspace UART ownership and the
# client->driver->wire path) and is validated by the operator on silicon. XMC has no QEMU
# model, and a live publish also silences the sim TAP.

# Without a KOS_SVC_CONSOLE entry there is no handover to demonstrate and every printf below
# would take the kernel path, so the selected list is a precondition, not a preference.
set(_consoledemo_lists
  kickos_services_xmc4800relax
  kickos_services_xmc4800relax_console)
if(NOT KICKOS_SERVICE_LIST IN_LIST _consoledemo_lists)
  message(STATUS
    "KickOS: consoledemo skipped: KICKOS_SERVICE_LIST=${KICKOS_SERVICE_LIST} publishes no "
    "userspace console. Build it with -DKICKOS_SERVICE_LIST=kickos_services_xmc4800relax_console.")
  return()
endif()

kickos_add_app_target(consoledemo main.cc)
target_link_libraries(consoledemo PRIVATE kickos)
kickos_emit_image(consoledemo)
