# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Sustained-output witness for a published USB CDC console. Built only in an image whose
# service list actually provides one: without a CDC driver cap 0 is the kernel console
# and the app measures nothing.
if(NOT KICKOS_SERVICE_LIST MATCHES "_usbcdc$")
  return()
endif()

kickos_add_application(usbcdcwit
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
