# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# KICKOS_USER_HEAP_SIZE, the link symbol the chip scripts size .userheap by, and which leaf of
# an image defines it: an old leaf from the knob, or for KickOS::kernel its system target.

# kickos_heap_defsym(<out> <value>)
#   The ld argument defining KICKOS_USER_HEAP_SIZE as <value>, or nothing for an empty value,
#   which would otherwise reach ld as a malformed --defsym.
function(kickos_heap_defsym out value)
  set(_arg "")
  if(NOT value STREQUAL "")
    set(_arg "--defsym=KICKOS_USER_HEAP_SIZE=${value}")
  endif()
  set(${out} "${_arg}" PARENT_SCOPE)
endfunction()

# kickos_link_closure(<target> <out>)
#   The targets <target>'s link closure reaches, aliases resolved, through LINK_LIBRARIES and
#   every INTERFACE_LINK_LIBRARIES below. Reads the closure linked so far.
function(kickos_link_closure target out)
  set(_seen "")
  set(_todo ${target})
  set(_root TRUE)
  while(_todo)
    list(POP_FRONT _todo _item)
    if(_item MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE):([^$<>]+)>$")
      set(_item "${CMAKE_MATCH_2}")
    endif()
    if(NOT TARGET "${_item}")
      continue()
    endif()
    get_target_property(_real "${_item}" ALIASED_TARGET)
    if(NOT _real)
      set(_real "${_item}")
    endif()
    if(_real IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen "${_real}")
    if(_root)
      get_target_property(_links "${_real}" LINK_LIBRARIES)
      if(_links)
        list(APPEND _todo ${_links})
      endif()
      set(_root FALSE)
    endif()
    get_target_property(_links "${_real}" INTERFACE_LINK_LIBRARIES)
    if(_links)
      list(APPEND _todo ${_links})
    endif()
  endwhile()
  set(${out} "${_seen}" PARENT_SCOPE)
endfunction()

# kickos_image_leaves(<target> <out>)
#   The leaves <target>'s link closure reaches, as `kernel`, `kickos` and `kickos_cxx`, in-tree
#   or exported spelling alike. An image reaching `kernel` takes its heap from its system target;
#   one reaching an old leaf besides would define the symbol twice, and is refused.
function(kickos_image_leaves target out)
  kickos_link_closure(${target} _closure)
  set(_leaves "")
  foreach(_real IN LISTS _closure)
    if(_real STREQUAL "kickos_kernel_leaf" OR _real STREQUAL "KickOS::kernel")
      list(APPEND _leaves kernel)
    elseif(_real STREQUAL "kickos" OR _real STREQUAL "KickOS::kickos")
      list(APPEND _leaves kickos)
    elseif(_real STREQUAL "kickos_cxx" OR _real STREQUAL "KickOS::kickos_cxx")
      list(APPEND _leaves kickos_cxx)
    endif()
  endforeach()
  list(LENGTH _leaves _count)
  if("kernel" IN_LIST _leaves AND _count GREATER 1)
    list(REMOVE_ITEM _leaves kernel)
    message(FATAL_ERROR "${target}: links KickOS::kernel and the ${_leaves} leaf, which would "
      "each define KICKOS_USER_HEAP_SIZE, the first from its system target and the second from "
      "the board's knob. Link one leaf.")
  endif()
  set(${out} "${_leaves}" PARENT_SCOPE)
endfunction()
