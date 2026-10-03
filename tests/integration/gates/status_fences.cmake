# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The init's status seqlock fences (user/src/init_status.cc), read out of the kickos_user archive
# the init and every watcher link: a store-ordering barrier between the odd count and the first
# field in status_write, and a load-ordering barrier on each side of the field loads in
# status_read. Registered on the arches whose fences are barrier instructions, an armv8a and an
# armv7m body each; the operands are the ones that order the owed direction there, a bare `dmb`
# reading as `-`. It runs no image, so it carries the host label.

if(NOT TARGET kickos_user)
  return()
endif()

set(_status_store_ops "")
set(_status_load_ops "")
if(KICKOS_ARCH STREQUAL "armv8a")
  set(_status_store_ops "ish ishst sy st")
  set(_status_load_ops "ish ishld sy ld")
elseif(KICKOS_ARCH STREQUAL "armv7m")
  set(_status_store_ops "ish sy -")
  set(_status_load_ops "ish sy -")
endif()
if(NOT _status_store_ops STREQUAL "")
  add_test(NAME status_fences
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_status_fences.sh"
            "$<TARGET_FILE:kickos_user>" "${CMAKE_OBJDUMP}"
            "${_status_store_ops}" "${_status_load_ops}")
  kickos_host_gate(status_fences)
endif()
