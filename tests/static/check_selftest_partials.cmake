# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The selftest partials tests/integration/selftest_partials.cmake derives, over planted
# compositions, run under cmake -P:
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

file(MAKE_DIRECTORY "${SCRATCH}")

file(WRITE "${SCRATCH}/kernel.yaml" "tasks:\n  - name: main\n    entry: selftest_main\n")
file(WRITE "${SCRATCH}/driver.yaml" "tasks:\n  - name: console\n    driver: f4uartirq\n"
                                    "  - driver: second\n  - name: main\n    entry: selftest_main\n")
_selftest_driver_tasks("${SCRATCH}/kernel.yaml" _none)
_expect("a composition with no driver" "${_none}" "0")
_selftest_driver_tasks("${SCRATCH}/driver.yaml" _two)
_expect("a composition with two drivers" "${_two}" "2")

_selftest_derived_partials(1 4 1 _got)
_expect("a pool of an idle, the init, main and a driver" "${_got}" "caller_stack_overlap")
_selftest_derived_partials(1 5 1 _got)
_expect("a pool with a slot past main and a driver" "${_got}" "")
_selftest_derived_partials(0 4 2 _got)
_expect("a pool of two idles, the init and main" "${_got}" "caller_stack_overlap")

_selftest_main_caps(9 4 _got)
_expect("main's free caps at 9 grants, 4 seated" "${_got}" "6")
_selftest_main_caps(6 4 _got)
_expect("main's free caps at 6 grants, 4 seated" "${_got}" "3")

_selftest_main_tasks(7 1 0 _got)
_expect("main's free tasks of 7, one core, no driver" "${_got}" "4")
_selftest_main_tasks(7 2 2 _got)
_expect("main's free tasks of 7, two cores, two drivers" "${_got}" "1")

file(READ "${KICKOS_SOURCE_DIR}/tests/integration/gates/selftest.cmake" _gate)
string(FIND "${_gate}" "_selftest_main_caps(\"\${KICKOS_MAX_SPAWN_GRANTS}\"" _width_at)
if(_width_at EQUAL -1)
  set(_failed "${_failed}\n  the gate does not size main's free caps by the child width")
endif()
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
