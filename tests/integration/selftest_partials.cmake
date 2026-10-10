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

# <max tasks> <cores> <driver tasks> <out>: the tasks main may create beside each kernel core's
# idle, the init's, its own and each driver task the image composes.
function(_selftest_main_tasks max_tasks cores driver_tasks out)
  math(EXPR _free "${max_tasks} - ${cores} - 2 - ${driver_tasks}")
  set(${out} ${_free} PARENT_SCOPE)
endfunction()

# <composition> <out>: how many driver tasks the composition file starts.
function(_selftest_driver_tasks composition out)
  file(STRINGS "${composition}" _drivers REGEX "^[ \t-]*driver:[ \t]")
  list(LENGTH _drivers _count)
  set(${out} ${_count} PARENT_SCOPE)
endfunction()

# <driver tasks> <max tasks> <cores> <out>: caller_stack_overlap where main may create no task for
# the data region.
function(_selftest_derived_partials driver_tasks max_tasks cores out)
  set(_partials "")
  _selftest_main_tasks(${max_tasks} ${cores} ${driver_tasks} _free)
  if(_free LESS 1)
    list(APPEND _partials caller_stack_overlap)
  endif()
  set(${out} "${_partials}" PARENT_SCOPE)
endfunction()
