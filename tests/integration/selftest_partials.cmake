# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The selftest partials an image's own sets and composition imply, included by
# tests/integration/gates/selftest.cmake.

# <width> <seated> <out>: the capability slots main has free for an arm, in a table <width> wide
# holding <seated> before any arm runs.
function(_selftest_main_caps width seated out)
  math(EXPR _free "${width} - ${seated}")
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
