# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The selftest partials and skips an image's own sets, composition and chip imply, included by
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

# <main.cc> <seat> <out>: where <seat> is OFF, the arms that skip on a doorbell keeping no seat,
# over main.cc and its sibling sources. A skip of that reason belongs to the function whose
# header is the nearest line above it at the namespace's indent, which TAP_ADD must register:
# a line there that heads no function, a declaration or a brace-initialised object, claims the
# skip for no arm.
function(_selftest_seat_skips main_cc seat out)
  get_filename_component(_dir "${main_cc}" DIRECTORY)
  file(GLOB _sources "${_dir}/*.cc")
  file(STRINGS "${main_cc}" _registered REGEX "TAP_ADD[A-Z_]*\\(\"[a-z0-9_]+\", *[A-Za-z0-9_]+\\)")
  set(_skips "")
  foreach(_src IN LISTS _sources)
    file(STRINGS "${_src}" _lines REGEX "^    [A-Za-z]|tap::skip\\(\"this doorbell has no seat")
    set(_fn "")
    foreach(_line IN LISTS _lines)
      if(_line MATCHES "tap::skip\\(\"this doorbell has no seat")
        if(_fn STREQUAL "")
          message(FATAL_ERROR "selftest: ${_src} skips on a seatless doorbell outside any function")
        endif()
        set(_arm "")
        foreach(_reg IN LISTS _registered)
          if(_reg MATCHES "TAP_ADD[A-Z_]*\\(\"([a-z0-9_]+)\", *${_fn}\\)")
            set(_arm "${CMAKE_MATCH_1}")
          endif()
        endforeach()
        if(_arm STREQUAL "")
          message(FATAL_ERROR "selftest: ${_src} skips on a seatless doorbell in '${_fn}', which "
                              "no TAP_ADD in main.cc registers")
        endif()
        list(APPEND _skips ${_arm})
      elseif(_line MATCHES "^    [A-Za-z]")
        set(_fn "")
        if(_line MATCHES "^    [^(=]*[^A-Za-z0-9_(]([A-Za-z_][A-Za-z0-9_]*)\\(")
          set(_fn "${CMAKE_MATCH_1}")
        endif()
      endif()
    endforeach()
  endforeach()
  if(NOT _skips)
    message(FATAL_ERROR "selftest: no arm skips on a seatless doorbell, so the reading is dead")
  endif()
  list(SORT _skips)
  if(seat)
    set(_skips "")
  endif()
  set(${out} "${_skips}" PARENT_SCOPE)
endfunction()
