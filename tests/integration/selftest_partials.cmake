# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The selftest partials an image's own sets and composition imply, included by
# tests/integration/gates/selftest.cmake and by tests/static/check_selftest_partials.cmake.

# <grants> <seated> <out>: the capability slots main has free for an arm. main's table is a
# spawned child's, KICKOS_MAX_SPAWN_GRANTS + 1 wide, never the board's supply.
function(_selftest_main_caps grants seated out)
  math(EXPR _free "${grants} + 1 - ${seated}")
  set(${out} ${_free} PARENT_SCOPE)
endfunction()

# <peers> <out>: caller_stack_overlap where the composition declares no peer for the data region.
function(_selftest_derived_partials peers out)
  set(_partials "")
  if(peers LESS 1)
    list(APPEND _partials caller_stack_overlap)
  endif()
  set(${out} "${_partials}" PARENT_SCOPE)
endfunction()
