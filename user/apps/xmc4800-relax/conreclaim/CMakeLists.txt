# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 console-reclaim test: an unprivileged holder of the U0C0 window garbles the
# console UART and faults; the MPU FAULT dump must still reach the wire. The window
# admits ONE holder, so a service list that publishes a userspace console driver makes
# the grant unobtainable; the kernel-owned console leaves it free. Enforcement is
# required for the fault to happen at all (main.cc #errors otherwise).
if(NOT (KICKOS_BOARD STREQUAL "xmc4800-relax" AND KICKOS_HAVE_MPU))
  return()
endif()
if(NOT KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  message(STATUS
    "KickOS: conreclaim skipped: KICKOS_SERVICE_LIST=${KICKOS_SERVICE_LIST} holds the U0C0 "
    "window. It needs BOTH -DKICKOS_SERVICE_LIST=kickos_services_none and "
    "-DKICKOS_ENABLE_SELFTEST=ON (it is a diagnostic app).")
  return()
endif()
# kickos_add_diagnostic_app returns silently without KICKOS_ENABLE_SELFTEST, so name the
# second requirement here: the service-list message alone sends the operator to a
# configure that produces no binary and says nothing.
if(NOT KICKOS_ENABLE_SELFTEST)
  message(STATUS
    "KickOS: conreclaim skipped: it is a diagnostic app and KICKOS_ENABLE_SELFTEST is off. "
    "Build it with -DKICKOS_ENABLE_SELFTEST=ON "
    "-DKICKOS_SERVICE_LIST=kickos_services_none.")
  return()
endif()

kickos_add_diagnostic_app(conreclaim
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET conreclaim)
  return() # diagnostics off -> app not built
endif()

# No CTest gate: PASS is an operator reading the panic banner on silicon (XMC has no
# QEMU model), which the in-env sim cannot reproduce.
