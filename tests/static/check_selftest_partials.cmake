# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The selftest partials tests/integration/selftest_partials.cmake derives, over planted sets and
# compositions and the real arms[], run under cmake -P:
#   cmake -DKICKOS_SOURCE_DIR=<repo root> -DSCRATCH=<dir> -P tests/static/check_selftest_partials.cmake

cmake_minimum_required(VERSION 3.24)
include("${KICKOS_SOURCE_DIR}/tests/integration/selftest_partials.cmake")

set(_failed "")
# <name> <got> <want>
function(_expect name got want)
  if(NOT "${got}" STREQUAL "${want}")
    set(_failed "${_failed}\n  ${name}: got [${got}], want [${want}]" PARENT_SCOPE)
  endif()
endfunction()

_selftest_starved_arms("${KICKOS_SOURCE_DIR}/user/apps/common/selftest/main.cc" _starved)
if(NOT "cross_task_block" IN_LIST _starved OR NOT "task_member_refusals" IN_LIST _starved)
  set(_failed "${_failed}\n  the real arms[] reads [${_starved}]")
endif()

file(MAKE_DIRECTORY "${SCRATCH}")
file(WRITE "${SCRATCH}/main.cc" "    StarvedArm const arms[] = {\n        {\"one\", t_one},\n#if X\n"
                                "        {\"two\", t_two},\n#endif\n    };\n    {\"three\", t_three},\n")
_selftest_starved_arms("${SCRATCH}/main.cc" _planted)
_expect("a planted arms[]" "${_planted}" "one;two")

file(WRITE "${SCRATCH}/kernel.yaml" "tasks:\n  - name: main\n    entry: selftest_main\n")
file(WRITE "${SCRATCH}/driver.yaml" "tasks:\n  - name: console\n    driver: f4uartirq\n"
                                    "  - driver: second\n  - name: main\n    entry: selftest_main\n")
_selftest_driver_tasks("${SCRATCH}/kernel.yaml" _none)
_expect("a composition with no driver" "${_none}" "0")
_selftest_driver_tasks("${SCRATCH}/driver.yaml" _two)
_expect("a composition with two drivers" "${_two}" "2")

_selftest_derived_partials("one;two" "x;two;y" 0 18 _got)
_expect("a starved arm expected to skip" "${_got}" "reservation_refused_skips")
_selftest_derived_partials("one;two" "x;y" 0 18 _got)
_expect("no starved arm expected to skip" "${_got}" "")
_selftest_derived_partials("one" "" 1 2 _got)
_expect("a pool of main and a driver" "${_got}" "caller_stack_overlap")
_selftest_derived_partials("one" "" 1 3 _got)
_expect("a pool with a slot past main and a driver" "${_got}" "")
_selftest_derived_partials("one" "" 0 2 _got)
_expect("a pool with a slot past main" "${_got}" "")

file(READ "${KICKOS_SOURCE_DIR}/tests/integration/gates/selftest.cmake" _gate)
string(FIND "${_gate}" "function(_selftest_image_sets " _sets_at)
string(FIND "${_gate}" "_selftest_derived_partials(" _derive_at)
string(FIND "${_gate}" "list(APPEND _partials \${_derived})" _append_at)
if(_sets_at EQUAL -1 OR _derive_at LESS _sets_at OR _append_at LESS _derive_at)
  set(_failed "${_failed}\n  _selftest_image_sets does not add the derived partials to an image's set")
endif()

if(_failed)
  message(FATAL_ERROR "selftest partials:${_failed}")
endif()
message(STATUS "selftest_partials: OK")
