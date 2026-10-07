# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The shared-kernel hardware predicate (cmake/smp_predicate.cmake) over synthetic declaration
# trees, one ctest case each. Included by the root CMakeLists, it registers the cases; each case
# runs it again in script mode, which writes the tree and calls the function the build calls:
#
#   cmake -DSRC=<repo root> -DTREE=<dir> -DARCH_FILE=<sets> -DCHIP_FILE=<sets> -DCORES=<n>
#         -DSHARED=<0|1> -P tests/static/check_smp_predicate.cmake
#
# <sets> is what that owner's smp.cmake declares, comma-separated: ARCH or CHIP for every
# property that owner's list names, ARCH0 or CHIP0 for the first alone, nothing for an empty
# file and NONE for no file. A refusal case passes on its phrases in the order the module prints
# them; an accepted case passes on the exit status.

if(CMAKE_SCRIPT_MODE_FILE)
  list(APPEND CMAKE_MODULE_PATH "${SRC}/cmake")
  include(smp_predicate)
  list(GET KICKOS_SMP_PROPS_ARCH 0 _arch0)
  list(GET KICKOS_SMP_PROPS_CHIP 0 _chip0)
  file(REMOVE_RECURSE "${TREE}")
  foreach(_owner ARCH CHIP)
    string(REPLACE "," ";" _sets "${${_owner}_FILE}")
    if(_sets STREQUAL "NONE")
      continue()
    endif()
    set(_body "")
    foreach(_set IN LISTS _sets)
      if(_set STREQUAL "ARCH0")
        set(_props "ARCH_SMP_${_arch0}")
      elseif(_set STREQUAL "CHIP0")
        set(_props "CHIP_SMP_${_chip0}")
      else()
        list(TRANSFORM KICKOS_SMP_PROPS_${_set} PREPEND "${_set}_SMP_" OUTPUT_VARIABLE _props)
      endif()
      foreach(_p IN LISTS _props)
        string(APPEND _body "set(KICKOS_${_p} 1)\n")
      endforeach()
    endforeach()
    set(_dir "${TREE}/arch/testfam/testarch")
    if(_owner STREQUAL "CHIP")
      set(_dir "${TREE}/arch/testfam/chip/testchip")
    endif()
    file(WRITE "${_dir}/smp.cmake" "${_body}")
  endforeach()
  kickos_smp_predicate(
    SOURCE_DIR   "${TREE}"
    ARCH         testarch
    ARCH_FAMILY  testfam
    CHIP         testchip
    BOARD        fixture-board
    NUM_CORES    "${CORES}"
    MODEL_SHARED "${SHARED}")
  return()
endif()

include(smp_predicate)
list(GET KICKOS_SMP_PROPS_ARCH 0 _sp_arch0)
list(GET KICKOS_SMP_PROPS_CHIP 0 _sp_chip0)
set(_sp_arch_file "arch/testfam/testarch/smp.cmake")
set(_sp_chip_file "arch/testfam/chip/testchip/smp.cmake")

# <case> <arch sets> <chip sets> <cores> <shared> [<phrase>...] [DENY <phrase>...]
function(_smp_predicate_case name arch chip cores shared)
  cmake_parse_arguments(SP "" "" "DENY" ${ARGN})
  _add_test(NAME smp_predicate_${name}
            COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
                    "-DTREE=${PROJECT_BINARY_DIR}/smp_predicate/${name}"
                    "-DARCH_FILE=${arch}" "-DCHIP_FILE=${chip}"
                    "-DCORES=${cores}" "-DSHARED=${shared}" -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
  set_tests_properties(smp_predicate_${name} PROPERTIES TIMEOUT 60 LABELS "host;tree")
  # CMake wraps a message at spaces, so a phrase matches across a line break.
  if(SP_UNPARSED_ARGUMENTS)
    list(JOIN SP_UNPARSED_ARGUMENTS ".*" _re)
    string(REPLACE " " "[ \n]+" _re "${_re}")
    set_tests_properties(smp_predicate_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${_re}")
  endif()
  if(SP_DENY)
    string(REPLACE " " "[ \n]+" _deny "${SP_DENY}")
    set_tests_properties(smp_predicate_${name} PROPERTIES FAIL_REGULAR_EXPRESSION "${_deny}")
  endif()
endfunction()

# Both owners declaring: the control every refusal below differs from in one clause.
_smp_predicate_case(alldecl ARCH CHIP 4 1)

# The arch complete and the part silent: every part-owned property named as the part's, none of
# the arch's.
set(_sp_part "")
foreach(_p IN LISTS KICKOS_SMP_PROPS_CHIP)
  list(APPEND _sp_part "the PART's" "Set KICKOS_CHIP_SMP_${_p} in" "${_sp_chip_file}")
endforeach()
_smp_predicate_case(partundecl ARCH NONE 4 1 ${_sp_part} DENY "KICKOS_ARCH_SMP_" "the ARCH's")

# An arch certifying a part's property, and a part certifying the ISA's: the two includes are
# separate scopes, so each direction is its own case.
_smp_predicate_case(overreach "ARCH,CHIP0" CHIP 4 1
  "${_sp_arch_file} sets KICKOS_CHIP_SMP_${_sp_chip0}," "property of a PART")
_smp_predicate_case(chipoverreach "" "CHIP,ARCH" 4 1
  "${_sp_chip_file} sets KICKOS_ARCH_SMP_${_sp_arch0}," "property of an ISA" "${_sp_arch_file}")

# The part restating a property its arch already declares, the value unchanged: only the scope
# the chip include runs in can refuse it.
_smp_predicate_case(chiprestate ARCH "CHIP,ARCH0" 4 1
  "${_sp_chip_file} sets KICKOS_ARCH_SMP_${_sp_arch0}," "property of an ISA")

# Nothing declared is accepted at one core, and at four under the AMP model: the refusal keys on
# the model and not the count.
_smp_predicate_case(onecore "" NONE 1 1)
_smp_predicate_case(ampmodel "" NONE 4 0)
