# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 console-reclaim test: an unprivileged holder of the U0C0 window garbles the
# console UART and faults; the MPU FAULT dump must still reach the wire.
#
# An OPERATOR on silicon is the reader: PASS is the
# "KERNEL PANIC: [conreclaim] PASS: the dead channel came back" line and the queued marker
# above it, both written while the channel was dead. Witnessed on the XMC4800-relax;
# main.cc records why kos_panic is the only terminal path left to an app here.

# The window admits ONE holder, so a service list that publishes a userspace console driver
# makes the grant unobtainable. The kernel-owned console leaves it free.
if(NOT KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  message(STATUS
    "KickOS: conreclaim skipped: KICKOS_SERVICE_LIST=${KICKOS_SERVICE_LIST} holds the U0C0 "
    "window. It needs -DKICKOS_SERVICE_LIST=kickos_services_none.")
  return()
endif()

kickos_add_app_target(conreclaim main.cc)
target_link_libraries(conreclaim PRIVATE kickos)
kickos_emit_image(conreclaim)
