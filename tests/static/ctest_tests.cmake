# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Flattens `ctest --show-only=json-v1` into one TAB-separated line per test, for
# check_test_labels.sh:
#
#   <name> <TAB> <,label,label,> <TAB> <0|1 DISABLED> <TAB> <program>
#
# Run as: cmake -DJSON=<file> -DOUT=<file> -P tests/static/ctest_tests.cmake
#
# Not awk: the command arguments here carry `;`, `"` and regex backslashes, and a property
# VALUE in this corpus spells "name" as a key, so a line-shaped parse of the pretty-printed
# JSON misreads it. string(JSON) is a real parser.
#
# NO FIELD IS EVER EMPTY. The caller reads these lines with IFS set to a tab, and a tab is
# IFS whitespace, so `read` collapses two adjacent tabs into one separator and shifts every
# later field left. Hence the comma fences on the label set (`,` when unlabelled) and @none.
#
# <program> is argv0 with the two wrappers this tree registers through peeled off, so the
# caller compares against the script or binary that actually runs:
#   - `cmake -E env [VAR=value]... prog args`  -> prog
#   - `cmake --build <dir>`                    -> @build
# It is @none when ctest emitted no command at all, which it does when argv0 does not
# resolve to a program on disk: an unbuilt tree, where every directly-registered image
# binary goes commandless. The caller refuses that rather than reading it as a class.

cmake_minimum_required(VERSION 3.24)

foreach(_v JSON OUT)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "ctest_tests.cmake: -D${_v}=<file> is required")
  endif()
endforeach()

file(READ "${JSON}" _json)
string(JSON _count LENGTH "${_json}" tests)
if(_count EQUAL 0)
  message(FATAL_ERROR "ctest_tests.cmake: ${JSON} declares zero tests")
endif()

set(_table "")
math(EXPR _last "${_count} - 1")
foreach(_i RANGE 0 ${_last})
  string(JSON _test GET "${_json}" tests ${_i})
  string(JSON _name GET "${_test}" name)

  set(_labels ",")
  set(_disabled 0)
  string(JSON _nprops ERROR_VARIABLE _ignored LENGTH "${_test}" properties)
  if(_nprops)
    math(EXPR _plast "${_nprops} - 1")
    foreach(_p RANGE 0 ${_plast})
      string(JSON _pname GET "${_test}" properties ${_p} name)
      if(_pname STREQUAL "LABELS")
        string(JSON _nlab LENGTH "${_test}" properties ${_p} value)
        math(EXPR _llast "${_nlab} - 1")
        foreach(_l RANGE 0 ${_llast})
          string(JSON _lab GET "${_test}" properties ${_p} value ${_l})
          string(APPEND _labels "${_lab},")
        endforeach()
      elseif(_pname STREQUAL "DISABLED")
        string(JSON _dis GET "${_test}" properties ${_p} value)
        if(_dis)
          set(_disabled 1)
        endif()
      endif()
    endforeach()
  endif()

  set(_prog "@none")
  string(JSON _nargs ERROR_VARIABLE _ignored LENGTH "${_test}" command)
  if(_nargs)
    set(_a 0)
    string(JSON _argv0 GET "${_test}" command 0)
    get_filename_component(_argv0_name "${_argv0}" NAME_WE)
    if(_argv0_name STREQUAL "cmake" AND _nargs GREATER 1)
      string(JSON _argv1 GET "${_test}" command 1)
      if(_argv1 STREQUAL "--build")
        set(_a -1)
      elseif(_argv1 STREQUAL "-E" AND _nargs GREATER 2)
        string(JSON _argv2 GET "${_test}" command 2)
        if(_argv2 STREQUAL "env")
          set(_a 3)
          while(_a LESS _nargs)
            string(JSON _arg GET "${_test}" command ${_a})
            if(NOT _arg MATCHES "^[A-Za-z_][A-Za-z0-9_]*=")
              break()
            endif()
            math(EXPR _a "${_a} + 1")
          endwhile()
        endif()
      endif()
    endif()
    if(_a EQUAL -1)
      set(_prog "@build")
    elseif(_a LESS _nargs)
      string(JSON _prog GET "${_test}" command ${_a})
    endif()
  endif()

  string(APPEND _table "${_name}\t${_labels}\t${_disabled}\t${_prog}\n")
endforeach()

file(WRITE "${OUT}" "${_table}")
