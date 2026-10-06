# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ROOT's capability-table width is the board's supply, KICKOS_CAP_TABLE_SUPPLY (its defconfig):
# root is the init, and admission counts what the init holds against that supply. Every spawned
# child gets KICKOS_CAP_CHILD_WIDTH, the grant-list floor KICKOS_MAX_SPAWN_GRANTS + 1: a full
# grant list lands at child indices 1..cap_count with no runtime check (cap.h), and nothing can
# ask for another width.
#
# Every input is a number the build states: the provisioning integers come from the generated
# CMake fragment and the structural constants from cmake/cap_geometry.cmake.

# The chunk geometry cap.h will compile for `slots`. MIRRORS the #if in cap.h: one exact-width
# chunk when the whole table fits the granule, else a ceiling count of granule-wide chunks.
# Only the reported footprint is derived here; the kernel computes its own.
function(_kickos_cap_geometry slots chunk out_chunks out_reserved_slots)
  if(NOT slots GREATER chunk)
    set(${out_chunks} 1 PARENT_SCOPE)
    set(${out_reserved_slots} "${slots}" PARENT_SCOPE)
    return()
  endif()
  math(EXPR _n "(${slots} + ${chunk} - 1) / ${chunk}")
  math(EXPR _r "${_n} * ${chunk}")
  set(${out_chunks} "${_n}" PARENT_SCOPE)
  set(${out_reserved_slots} "${_r}" PARENT_SCOPE)
endfunction()

# The slab's chunk count and .bss: one child-width run per holder, plus root's own widening
# on top. Every run holder is GUARANTEED the child width and every spawn asks for exactly
# that, so a spawn can never be refused for want of a chunk.
# MIRRORS KCAP_SLAB_CHUNKS in kernel/include/kickos/cap.h; the two must move together.
function(_kickos_cap_slab slots child chunk threads off_pool
                          out_bytes out_chunks out_child_chunks)
  _kickos_cap_geometry("${slots}" "${chunk}" _root_chunks _root_res)
  math(EXPR _chunk_slots "${_root_res} / ${_root_chunks}")
  math(EXPR _child_chunks "(${child} + ${_chunk_slots} - 1) / ${_chunk_slots}")
  math(EXPR _n "(${threads} + ${off_pool}) * ${_child_chunks} + ${_root_chunks} - ${_child_chunks}")
  math(EXPR _b "${_n} * ${_chunk_slots} * 8")
  set(${out_bytes} "${_b}" PARENT_SCOPE)
  set(${out_chunks} "${_n}" PARENT_SCOPE)
  set(${out_child_chunks} "${_child_chunks}" PARENT_SCOPE)
endfunction()

# Check root's width and the child floor against the board's supply, and forward both.
function(kickos_cap_table_resolve out_slots out_chunk out_child_width out_reply_max)
  set(_reserved "${KICKOS_CAP_FIRST_DYNAMIC}")
  set(_chunk "${KICKOS_CAP_CHUNK_TARGET}")
  set(_off_pool "${KICKOS_CAP_RUN_OFF_POOL}")
  set(_supply "${KICKOS_CAP_TABLE_SUPPLY}")
  set(_grants "${KICKOS_MAX_SPAWN_GRANTS}")
  set(_threads "${KICKOS_MAX_THREADS}")
  foreach(_v reserved chunk off_pool supply grants threads)
    if(NOT "${_${_v}}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR
        "KickOS: the capability-table width has no ${_v} value. The provisioning integers "
        "come from the generated kickos_config.cmake and the structural constants from "
        "cmake/cap_geometry.cmake; one of the two was not read before this call.")
    endif()
  endforeach()
  # Run holders are POOL SLOTS, and the pool has one more than the spawnable count because
  # kmain claims one for root before any spawn can run.
  # MIRRORS KICKOS_THREAD_SLOTS in kernel/include/kickos/config/system.h.
  math(EXPR _pool "${_threads} + 1")

  math(EXPR _floor "${_grants} + 1")
  # Of cap.h's three bounds on the child width, this is the one that does not hold by
  # construction: a small enough KICKOS_MAX_SPAWN_GRANTS puts a child's WHOLE table inside
  # the reserved range.
  if(NOT _floor GREATER _reserved)
    message(FATAL_ERROR
      "KickOS: a spawned child's capability table would be ${_floor} slot(s), which is "
      "entirely inside the ${_reserved}-slot reserved index range (KICKOS_CAP_FIRST_DYNAMIC), "
      "so no child could ever create a capability of its own. Every spawned child is seated "
      "at this width, KICKOS_MAX_SPAWN_GRANTS=${_grants} + 1; raise KICKOS_MAX_SPAWN_GRANTS.")
  endif()
  if(_floor GREATER _supply)
    math(EXPR _short "${_floor} - ${_supply}")
    message(FATAL_ERROR
      "KickOS: a full spawn grant list cannot fit root's capability table on board "
      "'${KICKOS_BOARD}': KICKOS_MAX_SPAWN_GRANTS=${_grants} needs ${_floor} slot(s) (grant i "
      "lands at child index i+1, and nothing checks it at runtime), and the board supplies "
      "${_supply}; short by ${_short}. Lower KICKOS_MAX_SPAWN_GRANTS, or raise the board's "
      "KICKOS_CAP_TABLE_SUPPLY if its RAM really can back it.")
  endif()

  # The most CONCURRENT inbound reply capabilities one thread may hold. It reserves no slot, and
  # a thread that may hold none could never serve a kos_call.
  set(_reply_max 1)
  # A child must keep at least one dynamic slot once the reply bound is spent, or peers alone
  # could fill its table.
  math(EXPR _child_own "${_floor} - ${_reserved} - ${_reply_max}")
  if(_child_own LESS 1)
    message(FATAL_ERROR
      "KickOS: a spawned child would keep ${_child_own} slot(s) of its own once an inbound "
      "reply capability is accounted: child width ${_floor} - ${_reserved} reserved - "
      "${_reply_max} inbound reply. Peers could then fill its table on their own, which is "
      "what KICKOS_CAP_REPLY_MAX exists to prevent. Raise KICKOS_MAX_SPAWN_GRANTS.")
  endif()

  set(_slots "${_supply}")
  _kickos_cap_geometry("${_slots}" "${_chunk}" _chunks _res_slots)
  _kickos_cap_slab("${_slots}" "${_floor}" "${_chunk}" "${_pool}" "${_off_pool}"
                   _bytes _slab_chunks _child_chunks)
  message(STATUS "KickOS: cap table = ${_slots} slot(s), the board's supply; ${_bytes} B .bss")
  # A run is reserved in whole chunks, so the last one's tail is paid for and unaddressable.
  if(_chunks EQUAL 1)
    message(STATUS "KickOS: cap table: 1 chunk of ${_res_slots}: the flat run, no directory, "
                   "no shift, nothing rounded up")
  else()
    math(EXPR _tail "${_res_slots} - ${_slots}")
    message(STATUS "KickOS: cap table: ${_chunks} chunks of ${_chunk} = ${_res_slots} slot(s) "
                   "reserved per run, ${_tail} of them unaddressable tail")
  endif()
  math(EXPR _root_extra "${_chunks} - ${_child_chunks}")
  message(STATUS "KickOS: cap table: child width ${_floor} slot(s) = ${_child_chunks} "
                 "chunk(s) guaranteed to each of ${_pool} + ${_off_pool} run holder(s), "
                 "plus ${_root_extra} chunk(s) for root's own widening")

  set(${out_slots} "${_slots}" PARENT_SCOPE)
  set(${out_chunk} "${_chunk}" PARENT_SCOPE)
  set(${out_child_width} "${_floor}" PARENT_SCOPE)
  set(${out_reply_max} "${_reply_max}" PARENT_SCOPE)
endfunction()
