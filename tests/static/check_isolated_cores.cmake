# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The isolated-core and root core-mask refusals (cmake/isolated_cores.cmake) over synthetic
# values, one ctest case each. Included by the root CMakeLists, it registers the cases; each case
# runs it again in script mode, which calls the function the build calls:
#
#   cmake -DCHECK=<isolated|root> -DMASK=<mask> -DCORES=<n> -DSHARED=<0|1>
#         -P tests/static/check_isolated_cores.cmake
#
# A refusal case passes on its phrase and the configuration it names; an accepted case passes on
# the exit status.

if(CMAKE_SCRIPT_MODE_FILE)
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/isolated_cores.cmake")
  if(CHECK STREQUAL "isolated")
    kickos_isolated_cores_check(ISOLATED "${MASK}" KERNEL_CORES "${CORES}" ORIGIN "the fixture")
  else()
    kickos_root_core_mask_check(MASK "${MASK}" KERNEL_CORES "${CORES}" SHARED "${SHARED}"
                                ORIGIN "the fixture")
  endif()
  return()
endif()

# <case> <check> <mask> <cores> <shared> <phrase the refusal names, or ACCEPT>. Each refusal sits
# beside a control differing in one clause.
set(_ic_cases
  core0_isolated          isolated 0x1  4 1 "core 0"
  core0_among_others      isolated 0x9  4 1 "core 0"
  core3_alone             isolated 0x8  4 1 ACCEPT
  bit_at_count            isolated 0x10 4 1 "does not schedule"
  bit_above_count         isolated 0x4  2 1 "does not schedule"
  bit4_on_eight           isolated 0x10 8 1 ACCEPT
  empty                   isolated 0x0  1 1 ACCEPT
  empty_wide              isolated 0x0  4 1 ACCEPT
  root_outside_shared     root     0x2  1 0 "outside the shared multicore model"
  root_core1_of_two       root     0x2  2 1 ACCEPT
  root_bit_at_count       root     0x4  2 1 "does not schedule"
  root_bit_above_count    root     0x10 4 1 "does not schedule"
  root_bit4_on_eight      root     0x10 8 1 ACCEPT
  root_empty              root     0x0  1 0 ACCEPT
  root_empty_shared       root     0x0  4 1 ACCEPT)
while(_ic_cases)
  list(POP_FRONT _ic_cases _ic_name _ic_check _ic_mask _ic_cores _ic_shared _ic_phrase)
  _add_test(NAME isolated_cores_${_ic_name}
            COMMAND "${CMAKE_COMMAND}" "-DCHECK=${_ic_check}" "-DMASK=${_ic_mask}"
                    "-DCORES=${_ic_cores}" "-DSHARED=${_ic_shared}"
                    -P "${CMAKE_CURRENT_LIST_FILE}")
  set_tests_properties(isolated_cores_${_ic_name} PROPERTIES TIMEOUT 60 LABELS "host;tree")
  if(NOT _ic_phrase STREQUAL "ACCEPT")
    # CMake wraps a message at spaces, so a phrase matches across a line break.
    string(REPLACE " " "[ \n]+" _ic_re "${_ic_phrase}.*the fixture")
    set_tests_properties(isolated_cores_${_ic_name} PROPERTIES PASS_REGULAR_EXPRESSION "${_ic_re}")
  endif()
endwhile()
