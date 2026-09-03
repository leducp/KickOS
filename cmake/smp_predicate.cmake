# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The shared-kernel hardware predicate: six properties that decide whether ONE kernel image
# can run across cores, declared by TWO owners and refused at configure time.
#
# BOTH declarations are included into THIS scope, so each owner's variables are taken out of the
# scope across the other's include and anything that comes back is refused. Enforced one way only,
# an arch certifies GIC version, routing and topology for parts nobody has seen, or one die's file
# satisfies the ISA's half for an arch that declares it nowhere.
#
# Keyed on the MODEL and not the count: an AMP image raises the count while needing none of
# the six, so a refusal keyed on the count locks AMP out of the count it needs.
#
# THE LAUNCH IS NOT ONE OF THE SIX, and does not belong here: two parts of one arch that meet
# all six can still release a secondary by different mechanisms.
#
# Driven twice, so a change to this signature breaks a gate: the root CMakeLists calls it for
# the real build, and tests/static/check_smp_predicate.sh calls it in script mode over
# synthetic declaration trees.

include_guard(GLOBAL)

# The properties, per owner, in the order the refusal lists them.
set(KICKOS_SMP_PROPS_ARCH COHERENT EXCLUSION IDENTITY)
set(KICKOS_SMP_PROPS_CHIP INTERRUPT SYMMETRIC TARGETING)

function(kickos_smp_predicate)
  cmake_parse_arguments(SP ""
    "SOURCE_DIR;ARCH;ARCH_FAMILY;CHIP;BOARD;NUM_CORES;MODEL_SHARED" "" ${ARGN})

  set(_w_COHERENT "coherent shared memory with no software maintenance")
  set(_w_EXCLUSION "an inter-core exclusion primitive")
  set(_w_IDENTITY "a per-core identity")
  set(_w_INTERRUPT "an inter-core interrupt")
  set(_w_SYMMETRIC "symmetric cores")
  set(_w_TARGETING "per-line interrupt targeting")

  # A family-less arch sits directly under arch/, which is where the sim's backend is.
  set(_arch_decl "${SP_SOURCE_DIR}/arch/${SP_ARCH_FAMILY}/${SP_ARCH}/smp.cmake")
  if(SP_ARCH_FAMILY STREQUAL SP_ARCH)
    set(_arch_decl "${SP_SOURCE_DIR}/arch/${SP_ARCH}/smp.cmake")
  endif()

  # The owner's whole set, spelled from the property lists: a message restating the names
  # outlives a rename and goes on naming a property the tree no longer has.
  set(_arch_names "")
  foreach(_p IN LISTS KICKOS_SMP_PROPS_ARCH)
    list(APPEND _arch_names "KICKOS_ARCH_SMP_${_p}")
  endforeach()
  list(JOIN _arch_names ", " _arch_names_text)
  set(_chip_names "")
  foreach(_p IN LISTS KICKOS_SMP_PROPS_CHIP)
    list(APPEND _chip_names "KICKOS_CHIP_SMP_${_p}")
  endforeach()
  list(JOIN _chip_names ", " _chip_names_text)

  # UNSET RATHER THAN ZEROED, both sets: a function scope masks the caller's value, so an
  # inherited KICKOS_*_SMP_* cannot read as a declaration, and DEFINED past an include names
  # exactly the file that wrote it. Every test below reads NOT of an undeclared property, which
  # holds on an unset one.
  foreach(_p IN LISTS KICKOS_SMP_PROPS_ARCH)
    unset(KICKOS_ARCH_SMP_${_p})
  endforeach()
  foreach(_p IN LISTS KICKOS_SMP_PROPS_CHIP)
    unset(KICKOS_CHIP_SMP_${_p})
  endforeach()

  if(EXISTS "${_arch_decl}")
    include("${_arch_decl}")
  endif()

  foreach(_p IN LISTS KICKOS_SMP_PROPS_CHIP)
    if(DEFINED KICKOS_CHIP_SMP_${_p})
      message(FATAL_ERROR
        "KickOS: ${_arch_decl} sets KICKOS_CHIP_SMP_${_p}, which declares ${_w_${_p}} for "
        "every part of arch '${SP_ARCH}' including the parts it has never seen. That is a "
        "property of a PART, not of an ISA. Delete the line and declare it in "
        "arch/${SP_ARCH_FAMILY}/chip/<chip>/smp.cmake, one file per part, where the "
        "interrupt controller and the cluster topology are known. An arch declares "
        "${_arch_names_text} and nothing else.")
    endif()
  endforeach()

  # HELD OUT OF THE CHIP INCLUDE'S SCOPE AND PUT BACK PAST IT, which is what makes the boundary
  # a boundary in both directions.
  foreach(_p IN LISTS KICKOS_SMP_PROPS_ARCH)
    set(_held_${_p} "${KICKOS_ARCH_SMP_${_p}}")
    unset(KICKOS_ARCH_SMP_${_p})
  endforeach()

  # A board with no chip can never declare the part's three, which is a refusal below rather
  # than a special case: nothing is standing in for the declaration.
  set(_chip_decl "")
  if(SP_CHIP)
    set(_chip_decl "${SP_SOURCE_DIR}/arch/${SP_ARCH_FAMILY}/chip/${SP_CHIP}/smp.cmake")
    if(EXISTS "${_chip_decl}")
      include("${_chip_decl}")
    endif()
  endif()

  foreach(_p IN LISTS KICKOS_SMP_PROPS_ARCH)
    if(DEFINED KICKOS_ARCH_SMP_${_p})
      message(FATAL_ERROR
        "KickOS: ${_chip_decl} sets KICKOS_ARCH_SMP_${_p}, which declares ${_w_${_p}}. That is "
        "a property of an ISA, not of a die: written from a part's file it satisfies the arch's "
        "half of the predicate for chip '${SP_CHIP}' alone, so arch '${SP_ARCH}' passes while "
        "declaring it nowhere and every other part of the arch inherits a claim no file makes "
        "for it. Delete the line and declare it in ${_arch_decl}, which is read for every part "
        "of the arch. A part declares ${_chip_names_text} and nothing else.")
    endif()
    set(KICKOS_ARCH_SMP_${_p} "${_held_${_p}}")
  endforeach()

  set(_missing "")
  foreach(_p IN LISTS KICKOS_SMP_PROPS_ARCH)
    if(NOT KICKOS_ARCH_SMP_${_p})
      list(APPEND _missing
        "${_w_${_p}}: the ARCH's, and arch '${SP_ARCH}' declares it nowhere. Set "
        "KICKOS_ARCH_SMP_${_p} in ${_arch_decl}.")
    endif()
  endforeach()
  foreach(_p IN LISTS KICKOS_SMP_PROPS_CHIP)
    if(NOT KICKOS_CHIP_SMP_${_p})
      if(_chip_decl)
        list(APPEND _missing
          "${_w_${_p}}: the PART's, and chip '${SP_CHIP}' declares it nowhere. Set "
          "KICKOS_CHIP_SMP_${_p} in ${_chip_decl}.")
      else()
        list(APPEND _missing
          "${_w_${_p}}: the PART's, and board '${SP_BOARD}' resolves no chip, so no file "
          "declares it. A part declares KICKOS_CHIP_SMP_${_p} in "
          "arch/<family>/chip/<chip>/smp.cmake.")
      endif()
    endif()
  endforeach()

  if(SP_NUM_CORES GREATER 1 AND SP_MODEL_SHARED AND _missing)
    list(JOIN _missing " " _missing_text)
    message(FATAL_ERROR
      "KickOS: KICKOS_NUM_CORES=${SP_NUM_CORES} under the SHARED-kernel model on arch "
      "'${SP_ARCH}' chip '${SP_CHIP}', which leaves properties of a shared kernel "
      "UNDECLARED. ${_missing_text} A part that fails the predicate runs AMP, one kernel per "
      "core over disjoint memory, rather than one image across cores. This refuses rather "
      "than warns because on a part with no cross-core primitive, bracketing with the LOCAL "
      "interrupt mask is a correctness bug and not a slow choice.")
  endif()
endfunction()
