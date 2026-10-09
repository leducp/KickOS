# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The address-space leave gate riding `taskleave`: a space installed on a peer core, which is
# the whole hazard.

if(NOT TARGET taskleave)
  return()
endif()

# The round count, read out of the app, so the judge requires every round's destroy.
set(_tl_main "${PROJECT_SOURCE_DIR}/user/apps/common/taskleave/main.cc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_tl_main}")
file(READ "${_tl_main}" _tl_src)
if(NOT _tl_src MATCHES "constexpr int ROUNDS = ([0-9]+)")
  message(FATAL_ERROR "taskleave: ROUNDS is not one plain integer in its main.cc")
endif()
set(_tl_rounds ${CMAKE_MATCH_1})
kickos_add_qemu_test(TARGET taskleave
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_taskleave.sh"
  ARGS ${_tl_rounds})
