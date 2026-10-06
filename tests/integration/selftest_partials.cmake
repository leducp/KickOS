# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The selftest partials an image's own sets and composition imply, included by
# tests/integration/gates/selftest.cmake and by tests/static/check_selftest_partials.cmake.

# <main.cc> <out>: the arms t_reservation_refused_skips runs starved, off its arms[].
function(_selftest_starved_arms main_cc out)
  file(READ "${main_cc}" _src)
  string(FIND "${_src}" "StarvedArm const arms[] = {" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "selftest: ${main_cc} holds no StarvedArm const arms[]")
  endif()
  string(SUBSTRING "${_src}" ${_at} -1 _rest)
  string(FIND "${_rest}" "};" _end)
  string(SUBSTRING "${_rest}" 0 ${_end} _block)
  string(REGEX MATCHALL "{\"[a-z0-9_]+\"," _entries "${_block}")
  set(_arms "")
  foreach(_entry IN LISTS _entries)
    string(REGEX REPLACE "^{\"([a-z0-9_]+)\",$" "\\1" _name "${_entry}")
    list(APPEND _arms ${_name})
  endforeach()
  if(NOT _arms)
    message(FATAL_ERROR "selftest: ${main_cc}'s arms[] names no arm")
  endif()
  set(${out} "${_arms}" PARENT_SCOPE)
endfunction()

# <composition> <out>: how many driver tasks the composition file starts.
function(_selftest_driver_tasks composition out)
  file(STRINGS "${composition}" _drivers REGEX "^[ \t-]*driver:[ \t]")
  list(LENGTH _drivers _count)
  set(${out} ${_count} PARENT_SCOPE)
endfunction()

# <starved> <skips> <driver tasks> <max tasks> <out>: reservation_refused_skips where an arm it
# runs starved is expected to skip, and caller_stack_overlap where the task pool cannot seat the
# data-region task beside main's and each driver task the image composes.
function(_selftest_derived_partials starved skips driver_tasks max_tasks out)
  set(_partials "")
  foreach(_arm IN LISTS starved)
    if(_arm IN_LIST skips)
      list(APPEND _partials reservation_refused_skips)
      break()
    endif()
  endforeach()
  math(EXPR _tasks "${driver_tasks} + 2")
  if(max_tasks LESS _tasks)
    list(APPEND _partials caller_stack_overlap)
  endif()
  set(${out} "${_partials}" PARENT_SCOPE)
endfunction()
