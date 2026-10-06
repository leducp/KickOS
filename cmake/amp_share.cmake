# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Validation for KICKOS_AMP_USER_SHARE_SIZE and KICKOS_AMP_USER_SHARE_UNCACHED, the tasks' share
# carved from the top of the region every node of an own-image AMP partition writes.
#
# tests/static/check_amp_share.sh drives these functions in script mode, over synthetic values,
# so each refusal stays a function and not an inline block.

function(kickos_amp_share_check)
  set(_one SHARE WINDOW UNCACHED TRANSLATES ORIGIN)
  cmake_parse_arguments(AS "" "${_one}" "" ${ARGN})
  if(NOT DEFINED AS_WINDOW OR AS_WINDOW STREQUAL "")
    message(FATAL_ERROR "kickos_amp_share_check needs WINDOW")
  endif()
  if(NOT DEFINED AS_SHARE OR AS_SHARE STREQUAL "")
    set(AS_SHARE 0)
  endif()
  math(EXPR _share "${AS_SHARE}")
  math(EXPR _window "${AS_WINDOW}")
  if(_share GREATER_EQUAL _window AND NOT _share EQUAL 0)
    message(FATAL_ERROR
      "KICKOS_AMP_USER_SHARE_SIZE is ${_share} bytes and the region every node writes, "
      "KICKOS_AMP_SHARED_SIZE, is ${_window}. The tasks' share is carved from that region's "
      "top, above the window, the doorbell's cells and the per-node counts the kernel places "
      "at its base, so a share as wide as the region, or past it, leaves the kernel nothing. "
      "Lower it, or raise KICKOS_AMP_SHARED_SIZE, in ${AS_ORIGIN}, and in every node's.")
  endif()
  if(AS_UNCACHED AND NOT AS_TRANSLATES)
    message(FATAL_ERROR
      "KICKOS_AMP_USER_SHARE_UNCACHED is set on a build that does not translate. The share's "
      "one memory type holds the kernel's own view of it too, and only a translating kernel "
      "maps that view with a type of its own: here the kernel reaches the share through its "
      "default memory map. Clear it in ${AS_ORIGIN}, and in every node's.")
  endif()
endfunction()

# The share is one region of the build's protection unit: MIN_REGION and POW2 are
# arch_mpu_min_region and arch_mpu_region_pow2 as the boot arena model scrapes them, and a
# MIN_REGION of 0 states no unit to describe.
function(kickos_amp_share_granule_check)
  set(_one SHARE BASE MIN_REGION POW2 ORIGIN)
  cmake_parse_arguments(AG "" "${_one}" "" ${ARGN})
  math(EXPR _share "${AG_SHARE}")
  if(_share EQUAL 0 OR "${AG_MIN_REGION}" STREQUAL "" OR AG_MIN_REGION EQUAL 0)
    return()
  endif()
  math(EXPR _base "${AG_BASE}")
  set(_align "${AG_MIN_REGION}")
  if(AG_POW2)
    set(_align "${_share}")
  endif()
  math(EXPR _whole "${_share} % ${AG_MIN_REGION}")
  math(EXPR _lead "${_base} % ${_align}")
  math(EXPR _pow2 "${_share} & (${_share} - 1)")
  if(NOT _whole EQUAL 0 OR NOT _lead EQUAL 0 OR (AG_POW2 AND NOT _pow2 EQUAL 0))
    message(FATAL_ERROR
      "KICKOS_AMP_USER_SHARE_SIZE is ${_share} bytes at ${AG_BASE}, which no one region of "
      "this build's protection unit describes: its granule is ${AG_MIN_REGION} bytes, and a "
      "region is a power of two on its own alignment where its pow2 flag is 1 (here "
      "${AG_POW2}). Root could not be seated with the share. Size it in ${AG_ORIGIN}, and in "
      "every node's.")
  endif()
endfunction()
