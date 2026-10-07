# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Validation for KICKOS_ISOLATED_CORES, the mask of cores left out of the default core set.
# THE GUARANTEE IS THAT NOTHING ARRIVES BY DEFAULT AND ONLY AN EXPLICIT MASK REACHES AN ISOLATED
# CORE, not that nothing else ever runs there: a mask naming one, alone or beside ordinary cores,
# is the opt-in and that core's picker then takes the thread.
#
# A FUNCTION AND NOT AN INLINE BLOCK IN THE ROOT LISTS FILE, so
# tests/static/check_isolated_cores.cmake can drive the same authority the build drives, over
# synthetic values, with a control beside every refusal. An inline FATAL_ERROR is reachable only
# by configuring a whole tree that actually fails, which is one arm and no controls.

function(kickos_isolated_cores_check)
  set(_one ISOLATED KERNEL_CORES ORIGIN)
  cmake_parse_arguments(IC "" "${_one}" "" ${ARGN})
  if(NOT DEFINED IC_ISOLATED OR IC_ISOLATED STREQUAL "")
    set(IC_ISOLATED 0)
  endif()
  if(NOT DEFINED IC_KERNEL_CORES OR IC_KERNEL_CORES STREQUAL "")
    message(FATAL_ERROR "kickos_isolated_cores_check needs KERNEL_CORES")
  endif()
  if(NOT DEFINED IC_ORIGIN OR IC_ORIGIN STREQUAL "")
    set(IC_ORIGIN "the board defconfig")
  endif()

  math(EXPR _boot "${IC_ISOLATED} & 1")
  if(NOT _boot EQUAL 0)
    message(FATAL_ERROR
      "KICKOS_ISOLATED_CORES names core 0, which may not be isolated. The default core set is "
      "every core this kernel drives less the isolated ones, and it is what the kernel, root "
      "and every thread that names no core run on, at spawn and on a zero-mask re-placement; "
      "holding the boot core out of the mask is "
      "what keeps that set non-empty at any core count. On a two-core part the only isolatable "
      "core is core 1. Clear bit 0 in ${IC_ORIGIN}.")
  endif()

  math(EXPR _undriven "${IC_ISOLATED} >> ${IC_KERNEL_CORES}")
  if(NOT _undriven EQUAL 0)
    message(FATAL_ERROR
      "KICKOS_ISOLATED_CORES names a core this kernel does not schedule: the mask is "
      "${IC_ISOLATED} and one kernel runs ${IC_KERNEL_CORES} cores, so only bits below "
      "${IC_KERNEL_CORES} name anything. Fix the mask in ${IC_ORIGIN}.")
  endif()
endfunction()

# Validation for KICKOS_ROOT_CORE_MASK, the core mask root is created with. Zero is the task's
# set; anything else is a placement only the shared multicore model has, and it must name
# cores the kernel drives, or root lands on no core that runs.
function(kickos_root_core_mask_check)
  set(_one MASK KERNEL_CORES SHARED ORIGIN)
  cmake_parse_arguments(RC "" "${_one}" "" ${ARGN})
  if(NOT DEFINED RC_MASK OR RC_MASK STREQUAL "")
    set(RC_MASK 0)
  endif()
  if(NOT DEFINED RC_KERNEL_CORES OR RC_KERNEL_CORES STREQUAL "")
    message(FATAL_ERROR "kickos_root_core_mask_check needs KERNEL_CORES")
  endif()
  if(NOT DEFINED RC_ORIGIN OR RC_ORIGIN STREQUAL "")
    set(RC_ORIGIN "the board defconfig")
  endif()
  math(EXPR _mask "${RC_MASK}")
  if(_mask EQUAL 0)
    return()
  endif()
  if(NOT RC_SHARED)
    message(FATAL_ERROR
      "KICKOS_ROOT_CORE_MASK is ${RC_MASK} outside the shared multicore model, where one "
      "kernel drives one core and root has no other to be placed on. Leave it 0 in "
      "${RC_ORIGIN}.")
  endif()
  math(EXPR _undriven "${_mask} >> ${RC_KERNEL_CORES}")
  if(NOT _undriven EQUAL 0)
    message(FATAL_ERROR
      "KICKOS_ROOT_CORE_MASK names a core this kernel does not schedule: the mask is "
      "${RC_MASK} and one kernel runs ${RC_KERNEL_CORES} cores, so only bits below "
      "${RC_KERNEL_CORES} name anything, and root would run on no core. Fix the mask in "
      "${RC_ORIGIN}.")
  endif()
endfunction()
